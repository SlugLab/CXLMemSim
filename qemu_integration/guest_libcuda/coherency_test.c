/*
 * CXL Type 2 GPU - CPU-GPU Coherency Test Program
 * Tests cache coherency between CPU and GPU through BAR regions
 *
 * Compile: gcc -O2 -o coherency_test coherency_test.c -L. -lcuda -pthread
 * Run: LD_LIBRARY_PATH=. ./coherency_test
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

/* CUDA types and error codes */
typedef int CUresult;
typedef int CUdevice;
typedef void* CUcontext;
typedef uint64_t CUdeviceptr;

#define CUDA_SUCCESS 0

/* CUDA Driver API declarations */
extern CUresult cuInit(unsigned int flags);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev);
extern CUresult cuCtxDestroy_v2(CUcontext ctx);
extern CUresult cuCtxSynchronize(void);
extern CUresult cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize);
extern CUresult cuMemFree_v2(CUdeviceptr dptr);
extern CUresult cuMemcpyHtoD_v2(CUdeviceptr dst, const void *src, size_t bytes);
extern CUresult cuMemcpyDtoH_v2(void *dst, CUdeviceptr src, size_t bytes);

#define CHECK_CUDA(call) do { \
    CUresult err = call; \
    if (err != CUDA_SUCCESS) { \
        printf("CUDA Error %d at %s:%d\n", err, __FILE__, __LINE__); \
        return -1; \
    } \
} while(0)

/* Test configuration */
#define TEST_SIZE       4096
#define NUM_ITERATIONS  100
#define NUM_THREADS     4

/* Shared state for multi-threaded tests */
static CUdeviceptr g_device_ptr = 0;
static volatile int g_barrier = 0;
static volatile int g_errors = 0;

/* Get current time in nanoseconds */
static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/*
 * Test 1: Basic coherency - write from CPU, read from CPU
 * Verifies that basic memory operations work correctly
 */
int test_basic_coherency(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr devPtr;
    uint8_t *hostSrc, *hostDst;
    int mismatch = 0;

    printf("\n=== Test 1: Basic Coherency ===\n");

    hostSrc = malloc(TEST_SIZE);
    hostDst = malloc(TEST_SIZE);
    if (!hostSrc || !hostDst) {
        printf("  ERROR: Host allocation failed\n");
        return -1;
    }

    /* Initialize source with pattern */
    for (size_t i = 0; i < TEST_SIZE; i++) {
        hostSrc[i] = (uint8_t)(i ^ 0xAA);
    }
    memset(hostDst, 0, TEST_SIZE);

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));
    CHECK_CUDA(cuMemAlloc_v2(&devPtr, TEST_SIZE));

    /* Write from CPU to device */
    uint64_t start = get_time_ns();
    CHECK_CUDA(cuMemcpyHtoD_v2(devPtr, hostSrc, TEST_SIZE));

    /* Synchronize to ensure write completes */
    CHECK_CUDA(cuCtxSynchronize());

    /* Read back from device to CPU */
    CHECK_CUDA(cuMemcpyDtoH_v2(hostDst, devPtr, TEST_SIZE));
    uint64_t end = get_time_ns();

    /* Verify data */
    for (size_t i = 0; i < TEST_SIZE; i++) {
        if (hostSrc[i] != hostDst[i]) {
            mismatch++;
            if (mismatch <= 5) {
                printf("  Mismatch at offset %zu: expected 0x%02x, got 0x%02x\n",
                       i, hostSrc[i], hostDst[i]);
            }
        }
    }

    printf("  Time: %lu ns\n", (unsigned long)(end - start));

    CHECK_CUDA(cuMemFree_v2(devPtr));
    CHECK_CUDA(cuCtxDestroy_v2(ctx));

    free(hostSrc);
    free(hostDst);

    if (mismatch == 0) {
        printf("  Result: PASSED\n");
        return 0;
    } else {
        printf("  Result: FAILED (%d mismatches)\n", mismatch);
        return -1;
    }
}

/*
 * Test 2: Multiple write-read cycles
 * Tests coherency across multiple iterations
 */
int test_multiple_cycles(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr devPtr;
    uint32_t *hostData;
    int errors = 0;

    printf("\n=== Test 2: Multiple Write-Read Cycles ===\n");

    hostData = malloc(TEST_SIZE);
    if (!hostData) {
        printf("  ERROR: Host allocation failed\n");
        return -1;
    }

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));
    CHECK_CUDA(cuMemAlloc_v2(&devPtr, TEST_SIZE));

    uint64_t total_time = 0;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        /* Fill with iteration-specific pattern */
        uint32_t pattern = 0xDEADBEEF ^ iter;
        for (size_t i = 0; i < TEST_SIZE / sizeof(uint32_t); i++) {
            hostData[i] = pattern + i;
        }

        uint64_t start = get_time_ns();

        /* Write to device */
        CHECK_CUDA(cuMemcpyHtoD_v2(devPtr, hostData, TEST_SIZE));
        CHECK_CUDA(cuCtxSynchronize());

        /* Clear host buffer */
        memset(hostData, 0, TEST_SIZE);

        /* Read back */
        CHECK_CUDA(cuMemcpyDtoH_v2(hostData, devPtr, TEST_SIZE));

        uint64_t end = get_time_ns();
        total_time += (end - start);

        /* Verify */
        for (size_t i = 0; i < TEST_SIZE / sizeof(uint32_t); i++) {
            uint32_t expected = pattern + i;
            if (hostData[i] != expected) {
                errors++;
                if (errors <= 5) {
                    printf("  Iter %d, offset %zu: expected 0x%08x, got 0x%08x\n",
                           iter, i * sizeof(uint32_t), expected, hostData[i]);
                }
            }
        }
    }

    printf("  Iterations: %d\n", NUM_ITERATIONS);
    printf("  Avg time per cycle: %lu ns\n", (unsigned long)(total_time / NUM_ITERATIONS));

    CHECK_CUDA(cuMemFree_v2(devPtr));
    CHECK_CUDA(cuCtxDestroy_v2(ctx));
    free(hostData);

    if (errors == 0) {
        printf("  Result: PASSED\n");
        return 0;
    } else {
        printf("  Result: FAILED (%d errors)\n", errors);
        return -1;
    }
}

/*
 * Test 3: Alternating access pattern
 * Tests coherency when CPU and "GPU" alternate access
 */
int test_alternating_access(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr devPtr;
    uint64_t *hostData;
    int errors = 0;

    printf("\n=== Test 3: Alternating Access Pattern ===\n");

    const size_t num_elements = TEST_SIZE / sizeof(uint64_t);
    hostData = malloc(TEST_SIZE);
    if (!hostData) {
        printf("  ERROR: Host allocation failed\n");
        return -1;
    }

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));
    CHECK_CUDA(cuMemAlloc_v2(&devPtr, TEST_SIZE));

    /* Initialize device memory */
    memset(hostData, 0, TEST_SIZE);
    CHECK_CUDA(cuMemcpyHtoD_v2(devPtr, hostData, TEST_SIZE));

    for (int iter = 0; iter < 50; iter++) {
        /* CPU writes even elements */
        for (size_t i = 0; i < num_elements; i += 2) {
            hostData[i] = (iter << 32) | i;
        }

        /* Write CPU's portion to device */
        for (size_t i = 0; i < num_elements; i += 2) {
            CHECK_CUDA(cuMemcpyHtoD_v2(devPtr + i * sizeof(uint64_t),
                                       &hostData[i], sizeof(uint64_t)));
        }

        /* Synchronize */
        CHECK_CUDA(cuCtxSynchronize());

        /* Simulate GPU writing odd elements (via HtoD) */
        for (size_t i = 1; i < num_elements; i += 2) {
            uint64_t gpu_val = ((uint64_t)iter << 32) | (i | 0x80000000);
            CHECK_CUDA(cuMemcpyHtoD_v2(devPtr + i * sizeof(uint64_t),
                                       &gpu_val, sizeof(uint64_t)));
        }

        /* Synchronize */
        CHECK_CUDA(cuCtxSynchronize());

        /* Read back all elements */
        uint64_t readback[num_elements];
        CHECK_CUDA(cuMemcpyDtoH_v2(readback, devPtr, TEST_SIZE));

        /* Verify both CPU and GPU portions */
        for (size_t i = 0; i < num_elements; i++) {
            uint64_t expected;
            if (i % 2 == 0) {
                expected = ((uint64_t)iter << 32) | i;
            } else {
                expected = ((uint64_t)iter << 32) | (i | 0x80000000);
            }
            if (readback[i] != expected) {
                errors++;
                if (errors <= 5) {
                    printf("  Iter %d, elem %zu: expected 0x%016lx, got 0x%016lx\n",
                           iter, i, (unsigned long)expected, (unsigned long)readback[i]);
                }
            }
        }
    }

    CHECK_CUDA(cuMemFree_v2(devPtr));
    CHECK_CUDA(cuCtxDestroy_v2(ctx));
    free(hostData);

    if (errors == 0) {
        printf("  Result: PASSED\n");
        return 0;
    } else {
        printf("  Result: FAILED (%d errors)\n", errors);
        return -1;
    }
}

/*
 * Test 4: Cache line boundary test
 * Tests coherency at cache line (64-byte) boundaries
 */
int test_cache_line_boundaries(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr devPtr;
    int errors = 0;

    printf("\n=== Test 4: Cache Line Boundary Test ===\n");

    const size_t buffer_size = 4096; /* Multiple cache lines */
    uint8_t *hostData = malloc(buffer_size);
    if (!hostData) {
        printf("  ERROR: Host allocation failed\n");
        return -1;
    }

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));
    CHECK_CUDA(cuMemAlloc_v2(&devPtr, buffer_size));

    /* Test writes at various offsets relative to cache line boundaries */
    int test_offsets[] = {0, 1, 31, 32, 63, 64, 65, 127, 128};
    int num_offsets = sizeof(test_offsets) / sizeof(test_offsets[0]);

    for (int t = 0; t < num_offsets; t++) {
        int offset = test_offsets[t];
        int write_size = 8; /* Write 8 bytes at a time */

        if (offset + write_size > (int)buffer_size) continue;

        /* Initialize pattern */
        uint64_t pattern = 0xCAFEBABE00000000ULL | (offset << 8) | t;
        memset(hostData, 0xCC, buffer_size);
        memcpy(hostData + offset, &pattern, sizeof(pattern));

        /* Write entire buffer to device */
        CHECK_CUDA(cuMemcpyHtoD_v2(devPtr, hostData, buffer_size));
        CHECK_CUDA(cuCtxSynchronize());

        /* Clear and read back */
        memset(hostData, 0, buffer_size);
        CHECK_CUDA(cuMemcpyDtoH_v2(hostData, devPtr, buffer_size));

        /* Verify the pattern */
        uint64_t readback;
        memcpy(&readback, hostData + offset, sizeof(readback));

        if (readback != pattern) {
            errors++;
            printf("  Offset %d: expected 0x%016lx, got 0x%016lx\n",
                   offset, (unsigned long)pattern, (unsigned long)readback);
        }

        /* Also verify surrounding bytes weren't corrupted */
        for (int i = 0; i < offset; i++) {
            if (hostData[i] != 0xCC) {
                errors++;
                if (errors <= 10) {
                    printf("  Pre-corruption at byte %d (test offset %d)\n", i, offset);
                }
            }
        }
    }

    CHECK_CUDA(cuMemFree_v2(devPtr));
    CHECK_CUDA(cuCtxDestroy_v2(ctx));
    free(hostData);

    if (errors == 0) {
        printf("  Result: PASSED\n");
        return 0;
    } else {
        printf("  Result: FAILED (%d errors)\n", errors);
        return -1;
    }
}

/*
 * Test 5: Large transfer coherency
 * Tests coherency with larger data sizes
 */
int test_large_transfers(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr devPtr;
    int errors = 0;

    printf("\n=== Test 5: Large Transfer Coherency ===\n");

    size_t test_sizes[] = {1024, 4096, 16384, 65536, 262144, 1048576}; /* 1KB to 1MB */
    int num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));

    for (int s = 0; s < num_sizes; s++) {
        size_t size = test_sizes[s];
        uint32_t *hostData = malloc(size);
        if (!hostData) continue;

        CHECK_CUDA(cuMemAlloc_v2(&devPtr, size));

        /* Fill with size-dependent pattern */
        uint32_t seed = (uint32_t)(size ^ 0x12345678);
        for (size_t i = 0; i < size / sizeof(uint32_t); i++) {
            hostData[i] = seed ^ (uint32_t)i;
        }

        uint64_t start = get_time_ns();

        /* Transfer to device */
        CHECK_CUDA(cuMemcpyHtoD_v2(devPtr, hostData, size));
        CHECK_CUDA(cuCtxSynchronize());

        /* Clear and read back */
        memset(hostData, 0, size);
        CHECK_CUDA(cuMemcpyDtoH_v2(hostData, devPtr, size));

        uint64_t end = get_time_ns();

        /* Verify */
        int size_errors = 0;
        for (size_t i = 0; i < size / sizeof(uint32_t); i++) {
            uint32_t expected = seed ^ (uint32_t)i;
            if (hostData[i] != expected) {
                size_errors++;
                errors++;
            }
        }

        double bandwidth_mb_s = (double)(size * 2) / ((end - start) / 1000.0);
        printf("  Size %7zu bytes: %s (%.2f MB/s)\n",
               size,
               size_errors == 0 ? "PASS" : "FAIL",
               bandwidth_mb_s);

        CHECK_CUDA(cuMemFree_v2(devPtr));
        free(hostData);
    }

    CHECK_CUDA(cuCtxDestroy_v2(ctx));

    if (errors == 0) {
        printf("  Result: PASSED\n");
        return 0;
    } else {
        printf("  Result: FAILED (%d total errors)\n", errors);
        return -1;
    }
}

/*
 * Test 6: Rapid small writes (stress test)
 * Tests coherency under rapid small write operations
 */
int test_rapid_small_writes(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr devPtr;
    int errors = 0;

    printf("\n=== Test 6: Rapid Small Writes ===\n");

    const int num_writes = 1000;
    const size_t buffer_size = num_writes * sizeof(uint32_t);
    uint32_t *hostData = malloc(buffer_size);
    if (!hostData) {
        printf("  ERROR: Host allocation failed\n");
        return -1;
    }

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));
    CHECK_CUDA(cuMemAlloc_v2(&devPtr, buffer_size));

    /* Initialize to zero */
    memset(hostData, 0, buffer_size);
    CHECK_CUDA(cuMemcpyHtoD_v2(devPtr, hostData, buffer_size));

    uint64_t start = get_time_ns();

    /* Perform many small writes */
    for (int i = 0; i < num_writes; i++) {
        uint32_t value = i | 0xA5000000;
        CHECK_CUDA(cuMemcpyHtoD_v2(devPtr + i * sizeof(uint32_t),
                                   &value, sizeof(uint32_t)));
    }

    CHECK_CUDA(cuCtxSynchronize());

    /* Read back all at once */
    CHECK_CUDA(cuMemcpyDtoH_v2(hostData, devPtr, buffer_size));

    uint64_t end = get_time_ns();

    /* Verify */
    for (int i = 0; i < num_writes; i++) {
        uint32_t expected = i | 0xA5000000;
        if (hostData[i] != expected) {
            errors++;
            if (errors <= 5) {
                printf("  Index %d: expected 0x%08x, got 0x%08x\n",
                       i, expected, hostData[i]);
            }
        }
    }

    printf("  Writes: %d\n", num_writes);
    printf("  Total time: %lu ns\n", (unsigned long)(end - start));
    printf("  Avg per write: %lu ns\n", (unsigned long)((end - start) / num_writes));

    CHECK_CUDA(cuMemFree_v2(devPtr));
    CHECK_CUDA(cuCtxDestroy_v2(ctx));
    free(hostData);

    if (errors == 0) {
        printf("  Result: PASSED\n");
        return 0;
    } else {
        printf("  Result: FAILED (%d errors)\n", errors);
        return -1;
    }
}

int main(void)
{
    int failed = 0;

    printf("CXL Type 2 GPU - CPU-GPU Coherency Test Program\n");
    printf("================================================\n");

    /* Initialize CUDA */
    CUresult err = cuInit(0);
    if (err != CUDA_SUCCESS) {
        printf("ERROR: cuInit failed with error %d\n", err);
        printf("Make sure CXL Type 2 device is present and libcuda.so is loaded\n");
        return 1;
    }

    /* Run tests */
    if (test_basic_coherency() != 0) {
        printf("\nBasic coherency test FAILED\n");
        failed++;
    }

    if (test_multiple_cycles() != 0) {
        printf("\nMultiple cycles test FAILED\n");
        failed++;
    }

    if (test_alternating_access() != 0) {
        printf("\nAlternating access test FAILED\n");
        failed++;
    }

    if (test_cache_line_boundaries() != 0) {
        printf("\nCache line boundary test FAILED\n");
        failed++;
    }

    if (test_large_transfers() != 0) {
        printf("\nLarge transfer test FAILED\n");
        failed++;
    }

    if (test_rapid_small_writes() != 0) {
        printf("\nRapid small writes test FAILED\n");
        failed++;
    }

    /* Summary */
    printf("\n================================================\n");
    if (failed == 0) {
        printf("All coherency tests PASSED\n");
        return 0;
    } else {
        printf("%d test(s) FAILED\n", failed);
        return 1;
    }
}
