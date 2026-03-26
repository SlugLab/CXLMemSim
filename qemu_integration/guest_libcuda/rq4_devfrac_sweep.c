/*
 * RQ4 quick: Hash table device fraction sweep
 * Measures throughput as fraction of chain nodes in device memory varies.
 *
 * Compile: gcc -O2 -o rq4_devfrac_sweep rq4_devfrac_sweep.c -L. -lcuda -lrt -lm -Wl,-rpath,.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
#define CUDA_SUCCESS 0
#define CXL_BIAS_DEVICE 1

extern CUresult cuInit(unsigned int);
extern CUresult cuDeviceGet(CUdevice *, int);
extern CUresult cuCtxCreate_v2(CUcontext *, unsigned int, CUdevice);
extern int cxlCoherentAlloc(uint64_t, void **);
extern int cxlCoherentFree(void *);
extern int cxlSetBias(void *, uint64_t, int);
extern int cxlCoherentFence(void);
extern int cxlResetCoherencyStats(void);

typedef struct {
    uint64_t snoop_hits, snoop_misses, coherency_requests, back_invalidations;
    uint64_t writebacks, evictions, bias_flips;
    uint64_t device_bias_hits, host_bias_hits;
    uint64_t upgrades, downgrades, directory_entries;
} CXLCoherencyStats;
extern int cxlGetCoherencyStats(CXLCoherencyStats *);

/* Safe memset for BAR4 memory */
static void cxl_memset(volatile void *dst, int val, size_t n)
{
    volatile unsigned char *d = (volatile unsigned char *)dst;
    for (size_t i = 0; i < n; i++)
        d[i] = (unsigned char)val;
}

#define NUM_BUCKETS  500
#define NUM_OPS      10000
#define NUM_TRIALS   5

typedef struct {
    uint64_t key;
    uint64_t value;
    uint64_t next;    /* index+1 into chain array, 0 = end */
    uint8_t  pad[40]; /* fill to 64 bytes */
} HTNode;

static uint64_t xor_state = 0xDEADBEEF12345ULL;
static uint64_t xrand(void) {
    xor_state ^= xor_state << 13;
    xor_state ^= xor_state >> 7;
    xor_state ^= xor_state << 17;
    return xor_state;
}

static uint64_t time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a, vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

/*
 * Run hash table workload.  Buckets always in device memory.
 * dev_frac of chain nodes go to device memory, rest to host.
 */
static double run_ht(double dev_frac, void *dev_pool, uint64_t dev_pool_size)
{
    /* Bucket array in device memory (first part of dev_pool) */
    uint64_t bucket_bytes = NUM_BUCKETS * sizeof(uint64_t);
    uint64_t *buckets = (uint64_t *)dev_pool;
    cxl_memset(buckets, 0, bucket_bytes);

    /* Chain nodes: device portion starts after buckets */
    uint64_t dev_chain_off = bucket_bytes;
    int chain_count = 0;

    /* Host chain node storage */
    HTNode *host_nodes[NUM_OPS];
    int host_count = 0;

    cxlCoherentFence();
    cxlResetCoherencyStats();

    uint64_t t0 = time_ns();

    for (int op = 0; op < NUM_OPS; op++) {
        uint64_t key = xrand();
        int is_put = (xrand() & 1);
        uint64_t bi = (key * 0xff51afd7ed558ccdULL >> 32) % NUM_BUCKETS;

        if (is_put) {
            /* Decide placement */
            int use_dev = ((double)(xrand() % 1000) / 1000.0) < dev_frac;
            HTNode *node;
            uint64_t node_idx;

            if (use_dev && dev_chain_off + sizeof(HTNode) <= dev_pool_size) {
                node = (HTNode *)((uint8_t *)dev_pool + dev_chain_off);
                cxl_memset(node, 0, sizeof(HTNode));
                dev_chain_off += sizeof(HTNode);
                node_idx = chain_count + 1; /* 1-based */
            } else {
                node = (HTNode *)calloc(1, sizeof(HTNode));
                if (!node) continue;
                host_nodes[host_count++] = node;
                node_idx = chain_count + 1;
            }

            node->key = key;
            node->value = (uint64_t)op;
            node->next = buckets[bi];
            buckets[bi] = node_idx;
            chain_count++;
        } else {
            /* GET: just read bucket (touch device memory) */
            volatile uint64_t h = buckets[bi];
            (void)h;
        }
    }

    uint64_t elapsed = time_ns() - t0;

    /* Cleanup host nodes */
    for (int i = 0; i < host_count; i++)
        free(host_nodes[i]);

    return (elapsed > 0) ? (double)NUM_OPS * 1e9 / (double)elapsed : 0;
}

int main(void)
{
    printf("=== RQ4: Hash Table Device Fraction Sweep ===\n\n");
    fflush(stdout);

    CUresult err = cuInit(0);
    if (err != CUDA_SUCCESS) { fprintf(stderr, "cuInit failed\n"); return 1; }
    CUdevice dev; cuDeviceGet(&dev, 0);
    CUcontext ctx; cuCtxCreate_v2(&ctx, 0, dev);
    printf("  CXL GPU device 0\n");
    printf("  Buckets=%d  Ops=%d  Trials=%d\n\n", NUM_BUCKETS, NUM_OPS, NUM_TRIALS);
    fflush(stdout);

    /* Allocate device pool large enough for buckets + all chain nodes */
    uint64_t pool_sz = NUM_BUCKETS * sizeof(uint64_t) + (uint64_t)NUM_OPS * sizeof(HTNode);
    void *dev_pool = NULL;
    if (cxlCoherentAlloc(pool_sz, &dev_pool) != CUDA_SUCCESS || !dev_pool) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }
    cxlSetBias(dev_pool, pool_sz, CXL_BIAS_DEVICE);

    double fracs[] = {0.0, 0.25, 0.50, 0.75, 1.0};
    int nfracs = 5;

    printf("%-12s %12s %12s %12s\n", "DevFrac", "Median(ops/s)", "p25", "p75");
    printf("%-12s %12s %12s %12s\n", "--------", "--------", "--------", "--------");
    fflush(stdout);

    for (int fi = 0; fi < nfracs; fi++) {
        uint64_t samples[NUM_TRIALS];
        for (int t = 0; t < NUM_TRIALS; t++) {
            xor_state = 0xDEADBEEF12345ULL + (uint64_t)t * 999 + (uint64_t)fi * 77;
            samples[t] = (uint64_t)run_ht(fracs[fi], dev_pool, pool_sz);
        }
        qsort(samples, NUM_TRIALS, sizeof(uint64_t), cmp_u64);
        printf("f=%-10.2f %12lu %12lu %12lu\n",
               fracs[fi],
               (unsigned long)samples[NUM_TRIALS/2],
               (unsigned long)samples[NUM_TRIALS/4],
               (unsigned long)samples[3*NUM_TRIALS/4]);
        fflush(stdout);
    }

    cxlCoherentFree(dev_pool);
    printf("\nDone.\n");
    return 0;
}
