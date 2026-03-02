/*
 * CXL Bias Mode Benchmark
 * Device-bias vs host-bias latency comparison
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "cxl_gpu_cmd.h"

/* CUDA types */
typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef uint64_t CUdeviceptr;

#define CUDA_SUCCESS 0

/* Coherency stats */
typedef struct {
    uint64_t snoop_hits;
    uint64_t snoop_misses;
    uint64_t coherency_requests;
    uint64_t back_invalidations;
    uint64_t writebacks;
    uint64_t evictions;
    uint64_t bias_flips;
    uint64_t device_bias_hits;
    uint64_t host_bias_hits;
    uint64_t upgrades;
    uint64_t downgrades;
    uint64_t directory_entries;
} CXLCoherencyStats;

/* External APIs */
extern CUresult cuInit(unsigned int flags);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev);

extern int cxlCoherentAlloc(uint64_t size, void **host_ptr);
extern int cxlCoherentFree(void *host_ptr);
extern int cxlCoherentFence(void);
extern int cxlSetBias(void *host_ptr, uint64_t size, int bias_mode);
extern int cxlGetBias(void *host_ptr, int *bias_mode);
extern int cxlBiasFlip(void *host_ptr, uint64_t size, int new_bias);
extern int cxlGetCoherencyStats(CXLCoherencyStats *stats);
extern int cxlResetCoherencyStats(void);

#define REGION_SIZE     (64 * 1024)  /* 64KB test region */
#define NUM_ITERATIONS  10000
#define STRIDE          64           /* Cache line stride */

static uint64_t time_diff_ns(struct timespec *start, struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000000000ULL +
           (end->tv_nsec - start->tv_nsec);
}

static void benchmark_cpu_writes(void *region, size_t size, const char *label)
{
    struct timespec t1, t2;
    volatile uint64_t *data = (volatile uint64_t *)region;
    size_t count = size / STRIDE;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        for (size_t i = 0; i < count; i++) {
            data[i * (STRIDE / sizeof(uint64_t))] = (uint64_t)iter + i;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);

    uint64_t total_ns = time_diff_ns(&t1, &t2);
    uint64_t ops = (uint64_t)NUM_ITERATIONS * count;
    printf("    %s CPU writes: %lu ns total, %lu ns/op (%lu ops)\n",
           label, (unsigned long)total_ns,
           (unsigned long)(total_ns / ops), (unsigned long)ops);
}

static void benchmark_cpu_reads(void *region, size_t size, const char *label)
{
    struct timespec t1, t2;
    volatile uint64_t *data = (volatile uint64_t *)region;
    size_t count = size / STRIDE;
    volatile uint64_t sink = 0;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        for (size_t i = 0; i < count; i++) {
            sink += data[i * (STRIDE / sizeof(uint64_t))];
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);

    (void)sink;

    uint64_t total_ns = time_diff_ns(&t1, &t2);
    uint64_t ops = (uint64_t)NUM_ITERATIONS * count;
    printf("    %s CPU reads:  %lu ns total, %lu ns/op (%lu ops)\n",
           label, (unsigned long)total_ns,
           (unsigned long)(total_ns / ops), (unsigned long)ops);
}

static void test_bias_set_get(void)
{
    printf("  [TEST] Bias set/get... ");

    void *region = NULL;
    int ret = cxlCoherentAlloc(REGION_SIZE, &region);
    if (ret != CUDA_SUCCESS || !region) {
        printf("FAIL: alloc failed\n");
        return;
    }

    /* Default should be host-biased */
    int bias = -1;
    ret = cxlGetBias(region, &bias);
    if (ret != CUDA_SUCCESS) {
        printf("FAIL: get bias failed\n");
        cxlCoherentFree(region);
        return;
    }

    /* Set to device-biased */
    ret = cxlSetBias(region, REGION_SIZE, CXL_BIAS_DEVICE);
    if (ret != CUDA_SUCCESS) {
        printf("FAIL: set bias failed\n");
        cxlCoherentFree(region);
        return;
    }

    ret = cxlGetBias(region, &bias);
    if (ret != CUDA_SUCCESS || bias != CXL_BIAS_DEVICE) {
        printf("FAIL: bias not device after set\n");
        cxlCoherentFree(region);
        return;
    }

    /* Flip back to host */
    ret = cxlBiasFlip(region, REGION_SIZE, CXL_BIAS_HOST);
    if (ret != CUDA_SUCCESS) {
        printf("FAIL: bias flip failed\n");
        cxlCoherentFree(region);
        return;
    }

    ret = cxlGetBias(region, &bias);
    if (ret != CUDA_SUCCESS || bias != CXL_BIAS_HOST) {
        printf("FAIL: bias not host after flip\n");
        cxlCoherentFree(region);
        return;
    }

    cxlCoherentFree(region);
    printf("PASS\n");
}

static void benchmark_host_bias(void)
{
    printf("\n  [BENCH] Host-biased mode:\n");

    void *region = NULL;
    int ret = cxlCoherentAlloc(REGION_SIZE, &region);
    if (ret != CUDA_SUCCESS || !region) {
        printf("    SKIP: alloc failed\n");
        return;
    }

    /* Set host-biased */
    cxlSetBias(region, REGION_SIZE, CXL_BIAS_HOST);
    cxlCoherentFence();

    cxlResetCoherencyStats();

    benchmark_cpu_writes(region, REGION_SIZE, "Host-bias");
    benchmark_cpu_reads(region, REGION_SIZE, "Host-bias");

    CXLCoherencyStats stats;
    cxlGetCoherencyStats(&stats);
    printf("    Host-bias stats: snoop_hits=%lu, host_bias_hits=%lu, "
           "device_bias_hits=%lu\n",
           (unsigned long)stats.snoop_hits,
           (unsigned long)stats.host_bias_hits,
           (unsigned long)stats.device_bias_hits);

    cxlCoherentFree(region);
}

static void benchmark_device_bias(void)
{
    printf("\n  [BENCH] Device-biased mode:\n");

    void *region = NULL;
    int ret = cxlCoherentAlloc(REGION_SIZE, &region);
    if (ret != CUDA_SUCCESS || !region) {
        printf("    SKIP: alloc failed\n");
        return;
    }

    /* Set device-biased */
    cxlSetBias(region, REGION_SIZE, CXL_BIAS_DEVICE);
    cxlCoherentFence();

    cxlResetCoherencyStats();

    benchmark_cpu_writes(region, REGION_SIZE, "Dev-bias");
    benchmark_cpu_reads(region, REGION_SIZE, "Dev-bias");

    CXLCoherencyStats stats;
    cxlGetCoherencyStats(&stats);
    printf("    Device-bias stats: snoop_hits=%lu, host_bias_hits=%lu, "
           "device_bias_hits=%lu\n",
           (unsigned long)stats.snoop_hits,
           (unsigned long)stats.host_bias_hits,
           (unsigned long)stats.device_bias_hits);

    cxlCoherentFree(region);
}

static void benchmark_bias_flip_overhead(void)
{
    printf("\n  [BENCH] Bias flip overhead:\n");

    void *region = NULL;
    int ret = cxlCoherentAlloc(REGION_SIZE, &region);
    if (ret != CUDA_SUCCESS || !region) {
        printf("    SKIP: alloc failed\n");
        return;
    }

    struct timespec t1, t2;
    int flips = 100;

    cxlResetCoherencyStats();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < flips; i++) {
        cxlBiasFlip(region, REGION_SIZE,
                     (i % 2 == 0) ? CXL_BIAS_DEVICE : CXL_BIAS_HOST);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);

    uint64_t total_ns = time_diff_ns(&t1, &t2);
    printf("    %d bias flips: %lu ns total, %lu ns/flip\n",
           flips, (unsigned long)total_ns,
           (unsigned long)(total_ns / flips));

    CXLCoherencyStats stats;
    cxlGetCoherencyStats(&stats);
    printf("    Flip stats: bias_flips=%lu, writebacks=%lu\n",
           (unsigned long)stats.bias_flips,
           (unsigned long)stats.writebacks);

    cxlCoherentFree(region);
}

static void benchmark_phase_pattern(void)
{
    printf("\n  [BENCH] Phase-based access pattern (CPU-write then GPU-read):\n");

    void *region = NULL;
    int ret = cxlCoherentAlloc(REGION_SIZE, &region);
    if (ret != CUDA_SUCCESS || !region) {
        printf("    SKIP: alloc failed\n");
        return;
    }

    struct timespec t1, t2;
    int phases = 50;
    volatile uint64_t *data = (volatile uint64_t *)region;
    size_t count = REGION_SIZE / sizeof(uint64_t);

    /* Without bias control: just do writes and reads */
    cxlResetCoherencyStats();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int phase = 0; phase < phases; phase++) {
        /* CPU write phase */
        for (size_t i = 0; i < count; i++) {
            data[i] = (uint64_t)phase + i;
        }
        cxlCoherentFence();

        /* Simulated GPU read phase (CPU reads standing in) */
        volatile uint64_t sink = 0;
        for (size_t i = 0; i < count; i++) {
            sink += data[i];
        }
        (void)sink;
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    uint64_t no_bias_ns = time_diff_ns(&t1, &t2);

    CXLCoherencyStats stats_no_bias;
    cxlGetCoherencyStats(&stats_no_bias);

    /* With bias control: flip at phase boundaries */
    cxlResetCoherencyStats();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int phase = 0; phase < phases; phase++) {
        /* Switch to host-bias for CPU write phase */
        cxlBiasFlip(region, REGION_SIZE, CXL_BIAS_HOST);

        for (size_t i = 0; i < count; i++) {
            data[i] = (uint64_t)phase + i;
        }
        cxlCoherentFence();

        /* Switch to device-bias for GPU read phase */
        cxlBiasFlip(region, REGION_SIZE, CXL_BIAS_DEVICE);

        volatile uint64_t sink = 0;
        for (size_t i = 0; i < count; i++) {
            sink += data[i];
        }
        (void)sink;
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    uint64_t bias_ns = time_diff_ns(&t1, &t2);

    CXLCoherencyStats stats_bias;
    cxlGetCoherencyStats(&stats_bias);

    printf("    Without bias control: %lu ns (%lu ns/phase)\n",
           (unsigned long)no_bias_ns, (unsigned long)(no_bias_ns / phases));
    printf("      coherency_reqs=%lu, back_inv=%lu\n",
           (unsigned long)stats_no_bias.coherency_requests,
           (unsigned long)stats_no_bias.back_invalidations);
    printf("    With bias control:    %lu ns (%lu ns/phase)\n",
           (unsigned long)bias_ns, (unsigned long)(bias_ns / phases));
    printf("      coherency_reqs=%lu, back_inv=%lu, bias_flips=%lu\n",
           (unsigned long)stats_bias.coherency_requests,
           (unsigned long)stats_bias.back_invalidations,
           (unsigned long)stats_bias.bias_flips);

    cxlCoherentFree(region);
}

int main(void)
{
    printf("=== CXL Bias Mode Benchmark ===\n\n");

    CUresult err = cuInit(0);
    if (err != CUDA_SUCCESS) {
        fprintf(stderr, "cuInit failed: %d\n", err);
        return 1;
    }

    CUdevice dev;
    err = cuDeviceGet(&dev, 0);
    if (err != CUDA_SUCCESS) {
        fprintf(stderr, "cuDeviceGet failed: %d\n", err);
        return 1;
    }

    CUcontext ctx;
    err = cuCtxCreate_v2(&ctx, 0, dev);
    if (err != CUDA_SUCCESS) {
        fprintf(stderr, "cuCtxCreate failed: %d\n", err);
        return 1;
    }

    printf("Running bias mode tests and benchmarks:\n");
    test_bias_set_get();
    benchmark_host_bias();
    benchmark_device_bias();
    benchmark_bias_flip_overhead();
    benchmark_phase_pattern();

    printf("\n=== Benchmark complete ===\n");
    return 0;
}
