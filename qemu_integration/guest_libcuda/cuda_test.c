/*
 * CXL Type 2 GPU - CUDA Test Program
 * Tests the libcuda.so shim that communicates with CXL Type 2 device
 *
 * Compile: make test
 * Run: ./cuda_test
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* CUDA types and error codes */
typedef int CUresult;
typedef int CUdevice;
typedef void* CUcontext;
typedef void* CUmodule;
typedef void* CUfunction;
typedef uint64_t CUdeviceptr;

#define CUDA_SUCCESS 0

/* CUDA Driver API declarations */
extern CUresult cuInit(unsigned int flags);
extern CUresult cuDriverGetVersion(int *version);
extern CUresult cuDeviceGetCount(int *count);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuDeviceGetName(char *name, int len, CUdevice dev);
extern CUresult cuDeviceTotalMem_v2(size_t *bytes, CUdevice dev);
extern CUresult cuDeviceGetAttribute(int *value, int attrib, CUdevice dev);
extern CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev);
extern CUresult cuCtxDestroy_v2(CUcontext ctx);
extern CUresult cuCtxSynchronize(void);
extern CUresult cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize);
extern CUresult cuMemFree_v2(CUdeviceptr dptr);
extern CUresult cuMemcpyHtoD_v2(CUdeviceptr dst, const void *src, size_t bytes);
extern CUresult cuMemcpyDtoH_v2(void *dst, CUdeviceptr src, size_t bytes);
extern CUresult cuMemGetInfo_v2(size_t *free, size_t *total);

/* Device attributes */
#define CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK 1
#define CU_DEVICE_ATTRIBUTE_WARP_SIZE 10
#define CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT 16
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR 75
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR 76

#define CHECK_CUDA(call) do { \
    CUresult err = call; \
    if (err != CUDA_SUCCESS) { \
        printf("CUDA Error %d at %s:%d\n", err, __FILE__, __LINE__); \
        return -1; \
    } \
} while(0)

int test_initialization(void)
{
    int version;
    int count;

    printf("=== Test: Initialization ===\n");

    CHECK_CUDA(cuInit(0));
    printf("  cuInit: OK\n");

    CHECK_CUDA(cuDriverGetVersion(&version));
    printf("  Driver version: %d.%d\n", version / 1000, (version % 1000) / 10);

    CHECK_CUDA(cuDeviceGetCount(&count));
    printf("  Device count: %d\n", count);

    if (count == 0) {
        printf("  ERROR: No devices found!\n");
        return -1;
    }

    return 0;
}

int test_device_info(void)
{
    CUdevice dev;
    char name[256];
    size_t totalMem;
    int value;

    printf("\n=== Test: Device Info ===\n");

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    printf("  Device handle: %d\n", dev);

    CHECK_CUDA(cuDeviceGetName(name, sizeof(name), dev));
    printf("  Device name: %s\n", name);

    CHECK_CUDA(cuDeviceTotalMem_v2(&totalMem, dev));
    printf("  Total memory: %zu MB\n", totalMem / (1024 * 1024));

    CHECK_CUDA(cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev));
    printf("  Compute capability major: %d\n", value);

    CHECK_CUDA(cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev));
    printf("  Compute capability minor: %d\n", value);

    CHECK_CUDA(cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, dev));
    printf("  Multiprocessor count: %d\n", value);

    CHECK_CUDA(cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK, dev));
    printf("  Max threads per block: %d\n", value);

    CHECK_CUDA(cuDeviceGetAttribute(&value, CU_DEVICE_ATTRIBUTE_WARP_SIZE, dev));
    printf("  Warp size: %d\n", value);

    return 0;
}

int test_context(void)
{
    CUdevice dev;
    CUcontext ctx;

    printf("\n=== Test: Context ===\n");

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));
    printf("  Context created: %p\n", ctx);

    CHECK_CUDA(cuCtxSynchronize());
    printf("  Context synchronized\n");

    CHECK_CUDA(cuCtxDestroy_v2(ctx));
    printf("  Context destroyed\n");

    return 0;
}

int test_memory(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr devPtr;
    size_t free_mem, total_mem;
    const size_t SIZE = 1024 * 1024; /* 1 MB */

    printf("\n=== Test: Memory ===\n");

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));

    CHECK_CUDA(cuMemGetInfo_v2(&free_mem, &total_mem));
    printf("  Memory: %zu MB free / %zu MB total\n",
           free_mem / (1024*1024), total_mem / (1024*1024));

    CHECK_CUDA(cuMemAlloc_v2(&devPtr, SIZE));
    printf("  Allocated %zu bytes at device address 0x%lx\n", SIZE, (unsigned long)devPtr);

    CHECK_CUDA(cuMemFree_v2(devPtr));
    printf("  Memory freed\n");

    CHECK_CUDA(cuCtxDestroy_v2(ctx));
    return 0;
}

int test_memcpy(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr devPtr;
    const size_t SIZE = 4096;
    uint8_t *hostSrc, *hostDst;
    int mismatch = 0;

    printf("\n=== Test: Memory Copy ===\n");

    hostSrc = malloc(SIZE);
    hostDst = malloc(SIZE);
    if (!hostSrc || !hostDst) {
        printf("  ERROR: Host allocation failed\n");
        return -1;
    }

    /* Initialize source with pattern */
    for (size_t i = 0; i < SIZE; i++) {
        hostSrc[i] = (uint8_t)(i & 0xFF);
    }
    memset(hostDst, 0, SIZE);

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));

    CHECK_CUDA(cuMemAlloc_v2(&devPtr, SIZE));
    printf("  Allocated device memory at 0x%lx\n", (unsigned long)devPtr);

    CHECK_CUDA(cuMemcpyHtoD_v2(devPtr, hostSrc, SIZE));
    printf("  Copied %zu bytes host -> device\n", SIZE);

    CHECK_CUDA(cuMemcpyDtoH_v2(hostDst, devPtr, SIZE));
    printf("  Copied %zu bytes device -> host\n", SIZE);

    /* Verify data */
    for (size_t i = 0; i < SIZE; i++) {
        if (hostSrc[i] != hostDst[i]) {
            mismatch++;
            if (mismatch <= 5) {
                printf("  Mismatch at offset %zu: expected 0x%02x, got 0x%02x\n",
                       i, hostSrc[i], hostDst[i]);
            }
        }
    }

    if (mismatch == 0) {
        printf("  Data verification: PASSED\n");
    } else {
        printf("  Data verification: FAILED (%d mismatches)\n", mismatch);
    }

    CHECK_CUDA(cuMemFree_v2(devPtr));
    CHECK_CUDA(cuCtxDestroy_v2(ctx));

    free(hostSrc);
    free(hostDst);

    return mismatch ? -1 : 0;
}

int main(void)
{
    int failed = 0;

    printf("CXL Type 2 GPU - CUDA Test Program\n");
    printf("===================================\n\n");

    if (test_initialization() != 0) {
        printf("\nInitialization test FAILED\n");
        return 1;
    }

    if (test_device_info() != 0) {
        printf("\nDevice info test FAILED\n");
        failed++;
    }

    if (test_context() != 0) {
        printf("\nContext test FAILED\n");
        failed++;
    }

    if (test_memory() != 0) {
        printf("\nMemory test FAILED\n");
        failed++;
    }

    if (test_memcpy() != 0) {
        printf("\nMemcpy test FAILED\n");
        failed++;
    }

    printf("\n===================================\n");
    if (failed == 0) {
        printf("All tests PASSED\n");
        return 0;
    } else {
        printf("%d test(s) FAILED\n", failed);
        return 1;
    }
}
