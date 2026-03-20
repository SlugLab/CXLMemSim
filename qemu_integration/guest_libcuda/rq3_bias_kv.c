/*
 * RQ3: Device-Biased vs Host-Biased Directory Indexing Performance
 *
 * Tests three workloads under device-bias, host-bias, and hybrid modes:
 *   1. Shared counter (extreme contention, two threads)
 *   2. Key-value store with Zipfian access distribution
 *   3. Producer-consumer ring buffer
 *
 * Compile:
 *   gcc -Wall -O2 -o rq3_bias_kv rq3_bias_kv.c -L. -lcuda -lrt -lm -lpthread -Wl,-rpath,.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <pthread.h>

#include "cxl_gpu_cmd.h"

/* ------------------------------------------------------------------ */
/* CUDA / CXL type declarations                                       */
/* ------------------------------------------------------------------ */
typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef uint64_t CUdeviceptr;

#define CUDA_SUCCESS 0

#ifndef CXL_BIAS_HOST
#define CXL_BIAS_HOST   0
#endif
#ifndef CXL_BIAS_DEVICE
#define CXL_BIAS_DEVICE 1
#endif

typedef struct {
    uint64_t snoop_hits, snoop_misses, coherency_requests, back_invalidations;
    uint64_t writebacks, evictions, bias_flips;
    uint64_t device_bias_hits, host_bias_hits;
    uint64_t upgrades, downgrades, directory_entries;
} CXLCoherencyStats;

extern CUresult cuInit(unsigned int flags);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev);

extern int cxlCoherentAlloc(uint64_t size, void **host_ptr);
extern int cxlCoherentFree(void *host_ptr);
extern void *cxlDeviceToHost(uint64_t dev_offset);
extern int cxlCoherentFence(void);
extern int cxlSetBias(void *host_ptr, uint64_t size, int bias_mode);
extern int cxlGetBias(void *host_ptr, int *bias_mode);
extern int cxlBiasFlip(void *host_ptr, uint64_t size, int new_bias);
extern int cxlGetCoherencyStats(CXLCoherencyStats *stats);
extern int cxlResetCoherencyStats(void);

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */
#define CACHE_LINE          64
#define NUM_TRIALS          10
#define COUNTER_OPS         100000
#define NUM_KEYS            10000
#define KV_OPS_PER_THREAD   50000
#define RING_SLOTS          1000
#define RING_RECORDS        100000
#define NUM_HOT_KEYS        100

/* ------------------------------------------------------------------ */
/* Key-value entry (fits one cache line)                              */
/* ------------------------------------------------------------------ */
typedef struct KVEntry {
    uint64_t key;
    uint64_t value;
    uint64_t version;
    uint8_t  padding[40];
} KVEntry;

/* Ring-buffer slot (one cache line) */
typedef struct RingSlot {
    uint64_t seq;          /* sequence number; 0 = empty */
    uint8_t  payload[56];
} RingSlot;

/* ------------------------------------------------------------------ */
/* Timing helpers                                                     */
/* ------------------------------------------------------------------ */
static inline uint64_t ts_to_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static inline uint64_t elapsed_ns(const struct timespec *t0,
                                   const struct timespec *t1)
{
    return ts_to_ns(t1) - ts_to_ns(t0);
}

/* ------------------------------------------------------------------ */
/* Sorting / percentile helpers                                       */
/* ------------------------------------------------------------------ */
static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static void report_percentiles(const char *label, uint64_t *samples, int n)
{
    qsort(samples, (size_t)n, sizeof(uint64_t), cmp_u64);
    int p25 = n / 4;
    int p50 = n / 2;
    int p75 = (3 * n) / 4;
    printf("    %-28s  p25=%lu  median=%lu  p75=%lu\n", label,
           (unsigned long)samples[p25],
           (unsigned long)samples[p50],
           (unsigned long)samples[p75]);
}

static void print_coherency_stats(const CXLCoherencyStats *s)
{
    printf("    coherency: snoop_hits=%lu  snoop_misses=%lu  "
           "bias_flips=%lu  writebacks=%lu\n",
           (unsigned long)s->snoop_hits,
           (unsigned long)s->snoop_misses,
           (unsigned long)s->bias_flips,
           (unsigned long)s->writebacks);
    printf("               dev_bias_hits=%lu  host_bias_hits=%lu  "
           "back_inv=%lu  dir_entries=%lu\n",
           (unsigned long)s->device_bias_hits,
           (unsigned long)s->host_bias_hits,
           (unsigned long)s->back_invalidations,
           (unsigned long)s->directory_entries);
}

/* ------------------------------------------------------------------ */
/* Zipfian distribution generator                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    int     n;           /* number of keys */
    double  theta;
    double *cdf;         /* cdf[0..n-1] */
    unsigned int seed;   /* per-generator PRNG state */
} ZipfGen;

static void zipf_init(ZipfGen *z, int n, double theta, unsigned int seed)
{
    z->n     = n;
    z->theta = theta;
    z->seed  = seed;
    z->cdf   = (double *)malloc(sizeof(double) * (size_t)n);
    if (!z->cdf) {
        fprintf(stderr, "FATAL: zipf_init malloc failed\n");
        exit(1);
    }

    /* Compute un-normalised CDF: sum of 1/(k+1)^theta */
    double sum = 0.0;
    for (int k = 0; k < n; k++) {
        sum += 1.0 / pow((double)(k + 1), theta);
        z->cdf[k] = sum;
    }
    /* Normalise */
    for (int k = 0; k < n; k++)
        z->cdf[k] /= sum;
}

static void zipf_free(ZipfGen *z)
{
    free(z->cdf);
    z->cdf = NULL;
}

/* Return a Zipfian-distributed key in [0, n) via binary search on CDF. */
static int zipf_next(ZipfGen *z)
{
    double u = (double)rand_r(&z->seed) / (double)RAND_MAX;
    /* Binary search for smallest k where cdf[k] >= u */
    int lo = 0, hi = z->n - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (z->cdf[mid] < u)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* ================================================================== */
/* EXPERIMENT 1: Shared Counter (extreme contention)                  */
/* ================================================================== */

typedef struct {
    volatile uint64_t *counter;
    int                ops;
    pthread_barrier_t *barrier;
} CounterArg;

static void *counter_thread(void *arg)
{
    CounterArg *ca = (CounterArg *)arg;
    pthread_barrier_wait(ca->barrier);
    for (int i = 0; i < ca->ops; i++)
        __sync_fetch_and_add(ca->counter, 1);
    return NULL;
}

static uint64_t run_counter_single(volatile uint64_t *counter, int ops)
{
    *counter = 0;
    cxlCoherentFence();

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < ops; i++)
        __sync_fetch_and_add(counter, 1);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    return elapsed_ns(&t0, &t1);
}

static uint64_t run_counter_dual(volatile uint64_t *counter, int ops_per_thread)
{
    *counter = 0;
    cxlCoherentFence();

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 2);

    CounterArg args[2] = {
        { counter, ops_per_thread, &barrier },
        { counter, ops_per_thread, &barrier },
    };

    pthread_t tids[2];
    struct timespec t0, t1;

    pthread_create(&tids[0], NULL, counter_thread, &args[0]);
    pthread_create(&tids[1], NULL, counter_thread, &args[1]);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_join(tids[0], NULL);
    pthread_join(tids[1], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    pthread_barrier_destroy(&barrier);
    return elapsed_ns(&t0, &t1);
}

static void experiment_shared_counter(void)
{
    printf("\n");
    printf("============================================================\n");
    printf("  EXPERIMENT 1: Shared Counter (extreme contention)\n");
    printf("============================================================\n");

    void *region = NULL;
    if (cxlCoherentAlloc(CACHE_LINE, &region) != CUDA_SUCCESS || !region) {
        fprintf(stderr, "  SKIP: coherent alloc failed\n");
        return;
    }
    volatile uint64_t *counter = (volatile uint64_t *)region;

    const char *modes[] = { "Device-Bias", "Host-Bias" };
    int biases[]        = { CXL_BIAS_DEVICE, CXL_BIAS_HOST };

    /* --- Single-thread baseline --- */
    for (int m = 0; m < 2; m++) {
        cxlSetBias(region, CACHE_LINE, biases[m]);
        cxlCoherentFence();

        uint64_t samples[NUM_TRIALS];
        for (int t = 0; t < NUM_TRIALS; t++) {
            cxlResetCoherencyStats();
            samples[t] = run_counter_single(counter, COUNTER_OPS);
        }
        /* Convert to ops/sec for reporting */
        uint64_t ops_sec[NUM_TRIALS];
        for (int t = 0; t < NUM_TRIALS; t++) {
            if (samples[t] == 0) samples[t] = 1;
            ops_sec[t] = (uint64_t)COUNTER_OPS * 1000000000ULL / samples[t];
        }
        char label[64];
        snprintf(label, sizeof(label), "1T %s ops/sec", modes[m]);
        report_percentiles(label, ops_sec, NUM_TRIALS);

        CXLCoherencyStats stats;
        cxlGetCoherencyStats(&stats);
        print_coherency_stats(&stats);
    }

    /* --- Two-thread contention --- */
    for (int m = 0; m < 2; m++) {
        cxlSetBias(region, CACHE_LINE, biases[m]);
        cxlCoherentFence();

        uint64_t samples[NUM_TRIALS];
        for (int t = 0; t < NUM_TRIALS; t++) {
            cxlResetCoherencyStats();
            samples[t] = run_counter_dual(counter, COUNTER_OPS);
        }
        uint64_t ops_sec[NUM_TRIALS];
        uint64_t total_ops = (uint64_t)COUNTER_OPS * 2;
        for (int t = 0; t < NUM_TRIALS; t++) {
            if (samples[t] == 0) samples[t] = 1;
            ops_sec[t] = total_ops * 1000000000ULL / samples[t];
        }
        char label[64];
        snprintf(label, sizeof(label), "2T %s ops/sec", modes[m]);
        report_percentiles(label, ops_sec, NUM_TRIALS);

        CXLCoherencyStats stats;
        cxlGetCoherencyStats(&stats);
        print_coherency_stats(&stats);
    }

    cxlCoherentFree(region);
}

/* ================================================================== */
/* EXPERIMENT 2: KV Store with Zipfian Distribution                   */
/* ================================================================== */

typedef struct {
    KVEntry           *kv;
    int                num_keys;
    int                ops;
    double             theta;
    unsigned int       seed;
    pthread_barrier_t *barrier;
} KVArg;

static void *kv_thread(void *arg)
{
    KVArg *ka = (KVArg *)arg;

    ZipfGen zg;
    zipf_init(&zg, ka->num_keys, ka->theta, ka->seed);

    pthread_barrier_wait(ka->barrier);

    for (int i = 0; i < ka->ops; i++) {
        int k = zipf_next(&zg);
        if (i & 1) {
            /* write */
            ka->kv[k].value   = (uint64_t)i;
            ka->kv[k].version = __sync_add_and_fetch(&ka->kv[k].version, 1);
        } else {
            /* read */
            volatile uint64_t sink = ka->kv[k].value;
            (void)sink;
        }
    }

    zipf_free(&zg);
    return NULL;
}

static uint64_t run_kv_trial(KVEntry *kv, int nkeys, double theta,
                              int ops_per_thread)
{
    /* Zero entries */
    memset(kv, 0, sizeof(KVEntry) * (size_t)nkeys);
    for (int k = 0; k < nkeys; k++)
        kv[k].key = (uint64_t)k;
    cxlCoherentFence();

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 2);

    KVArg args[2] = {
        { kv, nkeys, ops_per_thread, theta, 12345, &barrier },
        { kv, nkeys, ops_per_thread, theta, 67890, &barrier },
    };

    pthread_t tids[2];
    struct timespec t0, t1;

    pthread_create(&tids[0], NULL, kv_thread, &args[0]);
    pthread_create(&tids[1], NULL, kv_thread, &args[1]);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_join(tids[0], NULL);
    pthread_join(tids[1], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    pthread_barrier_destroy(&barrier);
    return elapsed_ns(&t0, &t1);
}

static void experiment_kv_zipfian(void)
{
    printf("\n");
    printf("============================================================\n");
    printf("  EXPERIMENT 2: KV Store with Zipfian Distribution\n");
    printf("============================================================\n");

    uint64_t alloc_sz = (uint64_t)NUM_KEYS * sizeof(KVEntry);
    void *region = NULL;
    if (cxlCoherentAlloc(alloc_sz, &region) != CUDA_SUCCESS || !region) {
        fprintf(stderr, "  SKIP: coherent alloc (%lu bytes) failed\n",
                (unsigned long)alloc_sz);
        return;
    }
    KVEntry *kv = (KVEntry *)region;

    /* Sweep theta from 0.0 to 1.5 in steps of 0.25 */
    for (double theta = 0.0; theta <= 1.501; theta += 0.25) {
        printf("\n  --- theta = %.2f ---\n", theta);

        /* Mode A: Device-Bias (all entries) */
        cxlSetBias(region, alloc_sz, CXL_BIAS_DEVICE);
        cxlCoherentFence();
        {
            uint64_t ops_sec[NUM_TRIALS];
            CXLCoherencyStats last_stats;
            for (int t = 0; t < NUM_TRIALS; t++) {
                cxlResetCoherencyStats();
                uint64_t ns = run_kv_trial(kv, NUM_KEYS, theta,
                                            KV_OPS_PER_THREAD);
                if (ns == 0) ns = 1;
                uint64_t total_ops = (uint64_t)KV_OPS_PER_THREAD * 2;
                ops_sec[t] = total_ops * 1000000000ULL / ns;
                cxlGetCoherencyStats(&last_stats);
            }
            report_percentiles("Device-Bias ops/sec", ops_sec, NUM_TRIALS);
            print_coherency_stats(&last_stats);
        }

        /* Mode B: Host-Bias (all entries) */
        cxlSetBias(region, alloc_sz, CXL_BIAS_HOST);
        cxlCoherentFence();
        {
            uint64_t ops_sec[NUM_TRIALS];
            CXLCoherencyStats last_stats;
            for (int t = 0; t < NUM_TRIALS; t++) {
                cxlResetCoherencyStats();
                uint64_t ns = run_kv_trial(kv, NUM_KEYS, theta,
                                            KV_OPS_PER_THREAD);
                if (ns == 0) ns = 1;
                uint64_t total_ops = (uint64_t)KV_OPS_PER_THREAD * 2;
                ops_sec[t] = total_ops * 1000000000ULL / ns;
                cxlGetCoherencyStats(&last_stats);
            }
            report_percentiles("Host-Bias ops/sec", ops_sec, NUM_TRIALS);
            print_coherency_stats(&last_stats);
        }

        /* Mode C: Hybrid -- hot keys device-biased, rest host-biased */
        /* First set entire region to host-bias, then override hot keys */
        cxlSetBias(region, alloc_sz, CXL_BIAS_HOST);
        for (int k = 0; k < NUM_HOT_KEYS && k < NUM_KEYS; k++) {
            cxlSetBias(&kv[k], sizeof(KVEntry), CXL_BIAS_DEVICE);
        }
        cxlCoherentFence();
        {
            uint64_t ops_sec[NUM_TRIALS];
            CXLCoherencyStats last_stats;
            for (int t = 0; t < NUM_TRIALS; t++) {
                cxlResetCoherencyStats();
                uint64_t ns = run_kv_trial(kv, NUM_KEYS, theta,
                                            KV_OPS_PER_THREAD);
                if (ns == 0) ns = 1;
                uint64_t total_ops = (uint64_t)KV_OPS_PER_THREAD * 2;
                ops_sec[t] = total_ops * 1000000000ULL / ns;
                cxlGetCoherencyStats(&last_stats);
            }
            report_percentiles("Hybrid ops/sec", ops_sec, NUM_TRIALS);
            print_coherency_stats(&last_stats);
        }
    }

    cxlCoherentFree(region);
}

/* ================================================================== */
/* EXPERIMENT 3: Producer-Consumer Ring Buffer                        */
/* ================================================================== */

typedef struct {
    volatile RingSlot *ring;
    int                slots;
    int                records;
    pthread_barrier_t *barrier;
} RingArg;

static void *producer_thread(void *arg)
{
    RingArg *ra = (RingArg *)arg;
    pthread_barrier_wait(ra->barrier);

    for (int i = 1; i <= ra->records; i++) {
        int idx = (i - 1) % ra->slots;
        volatile RingSlot *slot = &ra->ring[idx];

        /* Wait until consumer has consumed this slot (seq == 0) */
        while (slot->seq != 0)
            __sync_synchronize();

        /* Write payload (fill with sequence for verification) */
        uint8_t *p = (uint8_t *)slot->payload;
        for (int b = 0; b < 56; b++)
            p[b] = (uint8_t)(i & 0xFF);

        /* Publish: write seq last so consumer sees complete record */
        __sync_synchronize();
        slot->seq = (uint64_t)i;
    }
    return NULL;
}

static void *consumer_thread(void *arg)
{
    RingArg *ra = (RingArg *)arg;
    pthread_barrier_wait(ra->barrier);

    for (int i = 1; i <= ra->records; i++) {
        int idx = (i - 1) % ra->slots;
        volatile RingSlot *slot = &ra->ring[idx];

        /* Spin-wait for producer to publish this sequence */
        while (slot->seq != (uint64_t)i)
            __sync_synchronize();

        /* Read payload (volatile to prevent optimisation) */
        volatile uint8_t sum = 0;
        const uint8_t *p = (const uint8_t *)slot->payload;
        for (int b = 0; b < 56; b++)
            sum += p[b];
        (void)sum;

        /* Mark slot as consumed */
        __sync_synchronize();
        slot->seq = 0;
    }
    return NULL;
}

static uint64_t run_ring_trial(volatile RingSlot *ring, int slots,
                                int records)
{
    /* Clear all slots */
    memset((void *)ring, 0, sizeof(RingSlot) * (size_t)slots);
    cxlCoherentFence();

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 2);

    RingArg parg = { ring, slots, records, &barrier };
    RingArg carg = { ring, slots, records, &barrier };

    pthread_t prod, cons;
    struct timespec t0, t1;

    pthread_create(&prod, NULL, producer_thread, &parg);
    pthread_create(&cons, NULL, consumer_thread, &carg);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    pthread_barrier_destroy(&barrier);
    return elapsed_ns(&t0, &t1);
}

static void experiment_producer_consumer(void)
{
    printf("\n");
    printf("============================================================\n");
    printf("  EXPERIMENT 3: Producer-Consumer Ring Buffer\n");
    printf("============================================================\n");

    uint64_t alloc_sz = (uint64_t)RING_SLOTS * sizeof(RingSlot);
    void *region = NULL;
    if (cxlCoherentAlloc(alloc_sz, &region) != CUDA_SUCCESS || !region) {
        fprintf(stderr, "  SKIP: coherent alloc (%lu bytes) failed\n",
                (unsigned long)alloc_sz);
        return;
    }
    volatile RingSlot *ring = (volatile RingSlot *)region;

    uint64_t bytes_transferred = (uint64_t)RING_RECORDS * sizeof(RingSlot);

    const char *modes[] = { "Device-Bias", "Host-Bias" };
    int biases[]        = { CXL_BIAS_DEVICE, CXL_BIAS_HOST };

    for (int m = 0; m < 2; m++) {
        cxlSetBias(region, alloc_sz, biases[m]);
        cxlCoherentFence();

        printf("\n  --- %s ---\n", modes[m]);

        uint64_t bw_samples[NUM_TRIALS];   /* in bytes/sec */
        uint64_t ns_samples[NUM_TRIALS];
        CXLCoherencyStats last_stats;

        for (int t = 0; t < NUM_TRIALS; t++) {
            cxlResetCoherencyStats();
            uint64_t ns = run_ring_trial(ring, RING_SLOTS, RING_RECORDS);
            ns_samples[t] = ns;
            if (ns == 0) ns = 1;
            /* bandwidth in bytes/sec */
            bw_samples[t] = bytes_transferred * 1000000000ULL / ns;
            cxlGetCoherencyStats(&last_stats);
        }

        /* Report bandwidth in GB/s (divide by 1e9) */
        double gbps[NUM_TRIALS];
        uint64_t gbps_scaled[NUM_TRIALS];   /* milli-GB/s for percentile */
        for (int t = 0; t < NUM_TRIALS; t++) {
            gbps[t] = (double)bw_samples[t] / 1e9;
            gbps_scaled[t] = (uint64_t)(gbps[t] * 1000.0);  /* milli-GB/s */
        }

        /* Sort for percentiles */
        qsort(gbps_scaled, NUM_TRIALS, sizeof(uint64_t), cmp_u64);
        int p25 = NUM_TRIALS / 4;
        int p50 = NUM_TRIALS / 2;
        int p75 = (3 * NUM_TRIALS) / 4;
        printf("    %-28s  p25=%.3f  median=%.3f  p75=%.3f GB/s\n",
               modes[m],
               (double)gbps_scaled[p25] / 1000.0,
               (double)gbps_scaled[p50] / 1000.0,
               (double)gbps_scaled[p75] / 1000.0);

        report_percentiles("  latency (ns)", ns_samples, NUM_TRIALS);
        print_coherency_stats(&last_stats);
    }

    cxlCoherentFree(region);
}

/* ================================================================== */
/* Main                                                               */
/* ================================================================== */

int main(void)
{
    printf("============================================================\n");
    printf("  RQ3: Device-Biased vs Host-Biased Directory Performance\n");
    printf("============================================================\n");

    /* Initialise CUDA / CXL context */
    CUresult err = cuInit(0);
    if (err != CUDA_SUCCESS) {
        fprintf(stderr, "cuInit failed: %d\n", err);
        return 1;
    }

    CUdevice dev;
    CUcontext ctx;
    {
        extern CUresult cuDeviceGetCount(int *count);
        int dev_count = 0;
        cuDeviceGetCount(&dev_count);
        int found = 0;
        for (int di = 0; di < dev_count && !found; di++) {
            err = cuDeviceGet(&dev, di);
            if (err != CUDA_SUCCESS) continue;
            CUcontext try_ctx;
            err = cuCtxCreate_v2(&try_ctx, 0, dev);
            if (err == CUDA_SUCCESS) {
                ctx = try_ctx;
                found = 1;
                printf("  Using CXL GPU device %d\n", di);
            }
        }
        if (!found) {
            fprintf(stderr, "FATAL: no working CXL GPU found\n");
            return 1;
        }
    }

    printf("\n  Configuration:\n");
    printf("    NUM_TRIALS        = %d\n", NUM_TRIALS);
    printf("    COUNTER_OPS       = %d per thread\n", COUNTER_OPS);
    printf("    NUM_KEYS          = %d  (KV entries)\n", NUM_KEYS);
    printf("    KV_OPS_PER_THREAD = %d\n", KV_OPS_PER_THREAD);
    printf("    RING_SLOTS        = %d\n", RING_SLOTS);
    printf("    RING_RECORDS      = %d\n", RING_RECORDS);
    printf("    NUM_HOT_KEYS      = %d  (hybrid mode)\n", NUM_HOT_KEYS);

    /* Run all three experiments */
    experiment_shared_counter();
    experiment_kv_zipfian();
    experiment_producer_consumer();

    printf("\n============================================================\n");
    printf("  RQ3 experiments complete.\n");
    printf("============================================================\n");

    return 0;
}
