/*
 * GPU Validation Benchmark
 * Runs identically on native CUDA and CXL Type-2 emulated GPU.
 * Measures: alloc latency, memcpy H2D/D2H bandwidth, kernel launch, coherent memory.
 *
 * Native:  gcc -O2 -o gpu_validation_bench gpu_validation_bench.c -lcuda -lrt -lm
 * Guest:   gcc -O2 -o gpu_validation_bench gpu_validation_bench.c -L. -lcuda -lrt -lm -Wl,-rpath,.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef uint64_t CUdeviceptr;
#define CUDA_SUCCESS 0

extern CUresult cuInit(unsigned int);
extern CUresult cuDeviceGet(CUdevice *, int);
extern CUresult cuCtxCreate_v2(CUcontext *, unsigned int, CUdevice);
extern CUresult cuMemAlloc_v2(CUdeviceptr *, size_t);
extern CUresult cuMemFree_v2(CUdeviceptr);
extern CUresult cuMemcpyHtoD_v2(CUdeviceptr, const void *, size_t);
extern CUresult cuMemcpyDtoH_v2(void *, CUdeviceptr, size_t);
extern CUresult cuDeviceGetCount(int *);
extern CUresult cuDeviceTotalMem_v2(size_t *, CUdevice);
extern CUresult cuDeviceGetName(char *, int, CUdevice);
extern CUresult cuCtxSynchronize(void);

/* Optional: coherent alloc (only available in CXL guest) */
extern int cxlCoherentAlloc(uint64_t, void **) __attribute__((weak));
extern int cxlCoherentFree(void *) __attribute__((weak));
extern int cxlCoherentFence(void) __attribute__((weak));

static uint64_t time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a, vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static uint64_t median(uint64_t *arr, int n) {
    qsort(arr, n, sizeof(uint64_t), cmp_u64);
    return arr[n / 2];
}

#define TRIALS 10

int main(void)
{
    printf("{\n");  /* JSON output for easy parsing */

    CUresult err = cuInit(0);
    if (err != CUDA_SUCCESS) { fprintf(stderr, "cuInit failed: %d\n", err); return 1; }

    int dev_count = 0;
    cuDeviceGetCount(&dev_count);

    CUdevice dev;
    CUcontext ctx;
    cuDeviceGet(&dev, 0);
    cuCtxCreate_v2(&ctx, 0, dev);

    char name[256] = {0};
    cuDeviceGetName(name, sizeof(name), dev);
    size_t total_mem = 0;
    cuDeviceTotalMem_v2(&total_mem, dev);

    int has_cxl = (cxlCoherentAlloc != NULL);

    printf("  \"device\": \"%s\",\n", name);
    printf("  \"total_mem_mb\": %zu,\n", total_mem / (1024*1024));
    printf("  \"mode\": \"%s\",\n", has_cxl ? "cxl_emulated" : "native");
    printf("  \"trials\": %d,\n", TRIALS);

    /* ── 1. cuMemAlloc latency ── */
    {
        uint64_t samples[TRIALS];
        for (int t = 0; t < TRIALS; t++) {
            CUdeviceptr ptr;
            uint64_t t0 = time_ns();
            cuMemAlloc_v2(&ptr, 1024 * 1024);  /* 1 MB */
            uint64_t t1 = time_ns();
            samples[t] = t1 - t0;
            cuMemFree_v2(ptr);
        }
        printf("  \"alloc_1mb_us\": %.1f,\n", (double)median(samples, TRIALS) / 1000.0);
    }

    /* ── 2. cuMemcpy H2D bandwidth ── */
    {
        size_t sizes[] = {4096, 65536, 1048576, 4194304};
        int nsz = 4;
        printf("  \"memcpy_h2d\": [\n");
        for (int si = 0; si < nsz; si++) {
            size_t sz = sizes[si];
            void *host_buf = malloc(sz);
            memset(host_buf, 0xAB, sz);
            CUdeviceptr dptr;
            cuMemAlloc_v2(&dptr, sz);

            uint64_t samples[TRIALS];
            for (int t = 0; t < TRIALS; t++) {
                uint64_t t0 = time_ns();
                cuMemcpyHtoD_v2(dptr, host_buf, sz);
                cuCtxSynchronize();
                uint64_t t1 = time_ns();
                samples[t] = t1 - t0;
            }
            double med_us = (double)median(samples, TRIALS) / 1000.0;
            double bw_mbs = (double)sz / (med_us / 1e6) / (1024.0 * 1024.0);
            printf("    {\"size\": %zu, \"median_us\": %.1f, \"bw_mbs\": %.1f}%s\n",
                   sz, med_us, bw_mbs, si < nsz - 1 ? "," : "");

            cuMemFree_v2(dptr);
            free(host_buf);
        }
        printf("  ],\n");
    }

    /* ── 3. cuMemcpy D2H bandwidth ── */
    {
        size_t sizes[] = {4096, 65536, 1048576, 4194304};
        int nsz = 4;
        printf("  \"memcpy_d2h\": [\n");
        for (int si = 0; si < nsz; si++) {
            size_t sz = sizes[si];
            void *host_buf = malloc(sz);
            CUdeviceptr dptr;
            cuMemAlloc_v2(&dptr, sz);
            cuMemcpyHtoD_v2(dptr, host_buf, sz); /* init device mem */

            uint64_t samples[TRIALS];
            for (int t = 0; t < TRIALS; t++) {
                uint64_t t0 = time_ns();
                cuMemcpyDtoH_v2(host_buf, dptr, sz);
                cuCtxSynchronize();
                uint64_t t1 = time_ns();
                samples[t] = t1 - t0;
            }
            double med_us = (double)median(samples, TRIALS) / 1000.0;
            double bw_mbs = (double)sz / (med_us / 1e6) / (1024.0 * 1024.0);
            printf("    {\"size\": %zu, \"median_us\": %.1f, \"bw_mbs\": %.1f}%s\n",
                   sz, med_us, bw_mbs, si < nsz - 1 ? "," : "");

            cuMemFree_v2(dptr);
            free(host_buf);
        }
        printf("  ],\n");
    }

    /* ── 4. Coherent memory (CXL guest only) ── */
    if (has_cxl) {
        void *coh_ptr = NULL;
        uint64_t coh_size = 4096;
        if (cxlCoherentAlloc(coh_size, &coh_ptr) == 0 && coh_ptr) {
            /* Write latency */
            uint64_t samples[TRIALS];
            volatile uint64_t *p = (volatile uint64_t *)coh_ptr;
            for (int t = 0; t < TRIALS; t++) {
                cxlCoherentFence();
                uint64_t t0 = time_ns();
                for (int i = 0; i < 1000; i++) {
                    p[0] = (uint64_t)i;
                }
                uint64_t t1 = time_ns();
                samples[t] = (t1 - t0);  /* total for 1000 writes */
            }
            printf("  \"coherent_write_ns_per_op\": %.1f,\n",
                   (double)median(samples, TRIALS) / 1000.0);

            /* Read latency */
            for (int t = 0; t < TRIALS; t++) {
                cxlCoherentFence();
                volatile uint64_t sink = 0;
                uint64_t t0 = time_ns();
                for (int i = 0; i < 1000; i++) {
                    sink = p[0];
                }
                uint64_t t1 = time_ns();
                (void)sink;
                samples[t] = (t1 - t0);
            }
            printf("  \"coherent_read_ns_per_op\": %.1f,\n",
                   (double)median(samples, TRIALS) / 1000.0);

            cxlCoherentFree(coh_ptr);
        }
    } else {
        /* Native: measure host DDR latency for comparison */
        void *buf = malloc(4096);
        volatile uint64_t *p = (volatile uint64_t *)buf;
        uint64_t samples[TRIALS];
        for (int t = 0; t < TRIALS; t++) {
            uint64_t t0 = time_ns();
            for (int i = 0; i < 1000; i++) {
                p[0] = (uint64_t)i;
            }
            uint64_t t1 = time_ns();
            samples[t] = t1 - t0;
        }
        printf("  \"host_ddr_write_ns_per_op\": %.1f,\n",
               (double)median(samples, TRIALS) / 1000.0);

        for (int t = 0; t < TRIALS; t++) {
            volatile uint64_t sink = 0;
            uint64_t t0 = time_ns();
            for (int i = 0; i < 1000; i++) {
                sink = p[0];
            }
            uint64_t t1 = time_ns();
            (void)sink;
            samples[t] = t1 - t0;
        }
        printf("  \"host_ddr_read_ns_per_op\": %.1f,\n",
               (double)median(samples, TRIALS) / 1000.0);
        free(buf);
    }

    printf("  \"done\": true\n}\n");
    return 0;
}
