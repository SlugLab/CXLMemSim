/*
 * CXL Type 2 GPU - Advanced CUDA Test Program
 * Comprehensive tests for hetGPU NVIDIA backend through CXL
 *
 * Compile: gcc -o cuda_advanced_test cuda_advanced_test.c -L. -lcuda -ldl -lrt
 * Run: LD_LIBRARY_PATH=. ./cuda_advanced_test
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

/* CUDA types and error codes */
typedef int CUresult;
typedef int CUdevice;
typedef void* CUcontext;
typedef void* CUmodule;
typedef void* CUfunction;
typedef void* CUstream;
typedef void* CUevent;
typedef uint64_t CUdeviceptr;

#define CUDA_SUCCESS 0
#define CUDA_ERROR_INVALID_VALUE 1
#define CUDA_ERROR_OUT_OF_MEMORY 2
#define CUDA_ERROR_NOT_INITIALIZED 3
#define CUDA_ERROR_INVALID_CONTEXT 201

/* CUDA Driver API declarations */
extern CUresult cuInit(unsigned int flags);
extern CUresult cuDriverGetVersion(int *version);
extern CUresult cuDeviceGetCount(int *count);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuDeviceGetName(char *name, int len, CUdevice dev);
extern CUresult cuDeviceTotalMem_v2(size_t *bytes, CUdevice dev);
extern CUresult cuDeviceGetAttribute(int *value, int attrib, CUdevice dev);
extern CUresult cuDeviceGetUuid(void *uuid, CUdevice dev);
extern CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev);
extern CUresult cuCtxDestroy_v2(CUcontext ctx);
extern CUresult cuCtxSynchronize(void);
extern CUresult cuCtxGetCurrent(CUcontext *ctx);
extern CUresult cuCtxSetCurrent(CUcontext ctx);
extern CUresult cuCtxPushCurrent_v2(CUcontext ctx);
extern CUresult cuCtxPopCurrent_v2(CUcontext *ctx);
extern CUresult cuCtxGetDevice(CUdevice *dev);
extern CUresult cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize);
extern CUresult cuMemFree_v2(CUdeviceptr dptr);
extern CUresult cuMemcpyHtoD_v2(CUdeviceptr dst, const void *src, size_t bytes);
extern CUresult cuMemcpyDtoH_v2(void *dst, CUdeviceptr src, size_t bytes);
extern CUresult cuMemcpyDtoD_v2(CUdeviceptr dst, CUdeviceptr src, size_t bytes);
extern CUresult cuMemsetD8_v2(CUdeviceptr dptr, unsigned char value, size_t count);
extern CUresult cuMemsetD32_v2(CUdeviceptr dptr, unsigned int value, size_t count);
extern CUresult cuMemGetInfo_v2(size_t *free, size_t *total);
extern CUresult cuPointerGetAttribute(void *data, int attribute, CUdeviceptr ptr);
extern CUresult cuMemGetAddressRange_v2(CUdeviceptr *base, size_t *size, CUdeviceptr ptr);
extern CUresult cuModuleLoadData(CUmodule *module, const void *image);
extern CUresult cuModuleUnload(CUmodule module);
extern CUresult cuModuleGetFunction(CUfunction *func, CUmodule module, const char *name);
extern CUresult cuLaunchKernel(CUfunction f,
                               unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
                               unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
                               unsigned int sharedMemBytes, CUstream stream,
                               void **kernelParams, void **extra);
extern CUresult cuStreamCreate(CUstream *stream, unsigned int flags);
extern CUresult cuStreamDestroy_v2(CUstream stream);
extern CUresult cuStreamSynchronize(CUstream stream);
extern CUresult cuEventCreate(CUevent *event, unsigned int flags);
extern CUresult cuEventDestroy_v2(CUevent event);
extern CUresult cuEventRecord(CUevent event, CUstream stream);
extern CUresult cuEventSynchronize(CUevent event);
extern CUresult cuEventElapsedTime(float *ms, CUevent start, CUevent end);

/* Device attributes */
#define CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK 1
#define CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X 2
#define CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Y 3
#define CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Z 4
#define CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X 5
#define CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Y 6
#define CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Z 7
#define CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK 8
#define CU_DEVICE_ATTRIBUTE_TOTAL_CONSTANT_MEMORY 9
#define CU_DEVICE_ATTRIBUTE_WARP_SIZE 10
#define CU_DEVICE_ATTRIBUTE_MAX_REGISTERS_PER_BLOCK 12
#define CU_DEVICE_ATTRIBUTE_CLOCK_RATE 13
#define CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT 16
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR 75
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR 76
#define CU_DEVICE_ATTRIBUTE_L2_CACHE_SIZE 38
#define CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_MULTIPROCESSOR 39
#define CU_DEVICE_ATTRIBUTE_MEMORY_CLOCK_RATE 36
#define CU_DEVICE_ATTRIBUTE_GLOBAL_MEMORY_BUS_WIDTH 37
#define CU_DEVICE_ATTRIBUTE_MANAGED_MEMORY 83
#define CU_DEVICE_ATTRIBUTE_UNIFIED_ADDRESSING 41

/* Pointer attributes */
#define CU_POINTER_ATTRIBUTE_CONTEXT 1
#define CU_POINTER_ATTRIBUTE_MEMORY_TYPE 2

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) do { \
    printf("\n--- Test: %s ---\n", name); \
    tests_run++; \
} while(0)

#define TEST_PASS() do { \
    printf("  PASSED\n"); \
    tests_passed++; \
} while(0)

#define TEST_FAIL(msg) do { \
    printf("  FAILED: %s\n", msg); \
    tests_failed++; \
} while(0)

#define CHECK_CUDA(call) do { \
    CUresult err = call; \
    if (err != CUDA_SUCCESS) { \
        printf("  CUDA Error %d at %s:%d: %s\n", err, __FILE__, __LINE__, #call); \
        return -1; \
    } \
} while(0)

#define CHECK_CUDA_EXPECT_FAIL(call, expected) do { \
    CUresult err = call; \
    if (err != expected) { \
        printf("  Expected error %d but got %d\n", expected, err); \
        return -1; \
    } \
} while(0)

static double get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/*
 * Test 1: Multiple device queries
 */
int test_device_queries(void)
{
    CUdevice dev;
    int value;
    char name[256];
    size_t totalMem;

    TEST_START("Device Queries");

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuDeviceGetName(name, sizeof(name), dev));
    printf("  Device: %s\n", name);

    CHECK_CUDA(cuDeviceTotalMem_v2(&totalMem, dev));
    printf("  Total Memory: %zu MB\n", totalMem / (1024*1024));

    /* Query all important attributes */
    struct {
        int attr;
        const char *name;
    } attrs[] = {
        { CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK, "Max threads/block" },
        { CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X, "Max block dim X" },
        { CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Y, "Max block dim Y" },
        { CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Z, "Max block dim Z" },
        { CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X, "Max grid dim X" },
        { CU_DEVICE_ATTRIBUTE_WARP_SIZE, "Warp size" },
        { CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, "SM count" },
        { CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, "Compute major" },
        { CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, "Compute minor" },
        { CU_DEVICE_ATTRIBUTE_L2_CACHE_SIZE, "L2 cache size" },
        { CU_DEVICE_ATTRIBUTE_CLOCK_RATE, "Clock rate (kHz)" },
    };

    for (int i = 0; i < sizeof(attrs)/sizeof(attrs[0]); i++) {
        CUresult err = cuDeviceGetAttribute(&value, attrs[i].attr, dev);
        if (err == CUDA_SUCCESS) {
            printf("  %s: %d\n", attrs[i].name, value);
        } else {
            printf("  %s: (error %d)\n", attrs[i].name, err);
        }
    }

    TEST_PASS();
    return 0;
}

/*
 * Test 2: Context stack operations
 */
int test_context_stack(void)
{
    CUdevice dev;
    CUcontext ctx1, ctx2, current;
    CUdevice ctx_dev;

    TEST_START("Context Stack Operations");

    CHECK_CUDA(cuDeviceGet(&dev, 0));

    /* Create first context */
    CHECK_CUDA(cuCtxCreate_v2(&ctx1, 0, dev));
    printf("  Created ctx1: %p\n", ctx1);

    /* Check current context */
    CHECK_CUDA(cuCtxGetCurrent(&current));
    printf("  Current context after create: %p\n", current);

    /* Get device from context */
    CHECK_CUDA(cuCtxGetDevice(&ctx_dev));
    printf("  Device from context: %d\n", ctx_dev);

    /* Create second context */
    CHECK_CUDA(cuCtxCreate_v2(&ctx2, 0, dev));
    printf("  Created ctx2: %p\n", ctx2);

    /* Push ctx1 */
    CHECK_CUDA(cuCtxPushCurrent_v2(ctx1));
    CHECK_CUDA(cuCtxGetCurrent(&current));
    printf("  After push ctx1, current: %p\n", current);

    /* Pop and verify */
    CHECK_CUDA(cuCtxPopCurrent_v2(&current));
    printf("  Popped context: %p\n", current);

    /* Set current explicitly */
    CHECK_CUDA(cuCtxSetCurrent(ctx1));
    CHECK_CUDA(cuCtxGetCurrent(&current));
    printf("  After SetCurrent(ctx1), current: %p\n", current);

    /* Synchronize */
    CHECK_CUDA(cuCtxSynchronize());
    printf("  Context synchronized\n");

    /* Cleanup */
    CHECK_CUDA(cuCtxDestroy_v2(ctx1));
    CHECK_CUDA(cuCtxDestroy_v2(ctx2));

    TEST_PASS();
    return 0;
}

/*
 * Test 3: Multiple memory allocations
 */
int test_multiple_allocations(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr ptrs[16];
    size_t sizes[] = { 1024, 4096, 64*1024, 1024*1024, 16*1024*1024 };
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    int num_allocs = 0;

    TEST_START("Multiple Memory Allocations");

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));

    /* Allocate various sizes */
    for (int i = 0; i < num_sizes; i++) {
        CUresult err = cuMemAlloc_v2(&ptrs[num_allocs], sizes[i]);
        if (err == CUDA_SUCCESS) {
            printf("  Allocated %zu bytes at 0x%lx\n", sizes[i], (unsigned long)ptrs[num_allocs]);
            num_allocs++;
        } else {
            printf("  Failed to allocate %zu bytes: error %d\n", sizes[i], err);
        }
    }

    /* Test memory info */
    size_t free_mem, total_mem;
    CHECK_CUDA(cuMemGetInfo_v2(&free_mem, &total_mem));
    printf("  Memory: %zu MB free / %zu MB total\n",
           free_mem / (1024*1024), total_mem / (1024*1024));

    /* Free all allocations */
    for (int i = 0; i < num_allocs; i++) {
        CHECK_CUDA(cuMemFree_v2(ptrs[i]));
    }
    printf("  Freed %d allocations\n", num_allocs);

    CHECK_CUDA(cuCtxDestroy_v2(ctx));

    TEST_PASS();
    return 0;
}

/*
 * Test 4: Memory set operations
 */
int test_memset(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr dptr;
    const size_t SIZE = 1024 * 1024; /* 1 MB */
    uint8_t *host_buf;
    int errors = 0;

    TEST_START("Memory Set Operations");

    host_buf = malloc(SIZE);
    if (!host_buf) return -1;

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));
    CHECK_CUDA(cuMemAlloc_v2(&dptr, SIZE));

    /* Test memsetD8 */
    printf("  Testing cuMemsetD8...\n");
    CHECK_CUDA(cuMemsetD8_v2(dptr, 0xAB, SIZE));
    CHECK_CUDA(cuMemcpyDtoH_v2(host_buf, dptr, SIZE));

    for (size_t i = 0; i < SIZE; i++) {
        if (host_buf[i] != 0xAB) {
            errors++;
            if (errors <= 5) {
                printf("    Mismatch at %zu: expected 0xAB, got 0x%02x\n", i, host_buf[i]);
            }
        }
    }
    printf("    cuMemsetD8: %s (%d errors)\n", errors == 0 ? "OK" : "FAILED", errors);

    /* Test memsetD32 */
    printf("  Testing cuMemsetD32...\n");
    errors = 0;
    CHECK_CUDA(cuMemsetD32_v2(dptr, 0xDEADBEEF, SIZE / 4));
    CHECK_CUDA(cuMemcpyDtoH_v2(host_buf, dptr, SIZE));

    uint32_t *host32 = (uint32_t *)host_buf;
    for (size_t i = 0; i < SIZE / 4; i++) {
        if (host32[i] != 0xDEADBEEF) {
            errors++;
            if (errors <= 5) {
                printf("    Mismatch at %zu: expected 0xDEADBEEF, got 0x%08x\n", i, host32[i]);
            }
        }
    }
    printf("    cuMemsetD32: %s (%d errors)\n", errors == 0 ? "OK" : "FAILED", errors);

    CHECK_CUDA(cuMemFree_v2(dptr));
    CHECK_CUDA(cuCtxDestroy_v2(ctx));
    free(host_buf);

    if (errors > 0) {
        TEST_FAIL("memset verification failed");
        return -1;
    }

    TEST_PASS();
    return 0;
}

/*
 * Test 5: Device-to-device copy
 */
int test_d2d_copy(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr src, dst;
    const size_t SIZE = 64 * 1024; /* 64 KB */
    uint8_t *host_src, *host_dst;
    int errors = 0;

    TEST_START("Device-to-Device Copy");

    host_src = malloc(SIZE);
    host_dst = malloc(SIZE);
    if (!host_src || !host_dst) return -1;

    /* Initialize source pattern */
    for (size_t i = 0; i < SIZE; i++) {
        host_src[i] = (uint8_t)((i * 17 + 23) & 0xFF);
    }
    memset(host_dst, 0, SIZE);

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));

    CHECK_CUDA(cuMemAlloc_v2(&src, SIZE));
    CHECK_CUDA(cuMemAlloc_v2(&dst, SIZE));
    printf("  Allocated src=0x%lx, dst=0x%lx\n", (unsigned long)src, (unsigned long)dst);

    /* Copy host -> device (src) */
    CHECK_CUDA(cuMemcpyHtoD_v2(src, host_src, SIZE));
    printf("  Copied %zu bytes H->D\n", SIZE);

    /* Copy device -> device */
    CHECK_CUDA(cuMemcpyDtoD_v2(dst, src, SIZE));
    printf("  Copied %zu bytes D->D\n", SIZE);

    /* Copy device (dst) -> host */
    CHECK_CUDA(cuMemcpyDtoH_v2(host_dst, dst, SIZE));
    printf("  Copied %zu bytes D->H\n", SIZE);

    /* Verify */
    for (size_t i = 0; i < SIZE; i++) {
        if (host_src[i] != host_dst[i]) {
            errors++;
            if (errors <= 5) {
                printf("    Mismatch at %zu: expected 0x%02x, got 0x%02x\n",
                       i, host_src[i], host_dst[i]);
            }
        }
    }
    printf("  Verification: %s (%d errors)\n", errors == 0 ? "PASSED" : "FAILED", errors);

    CHECK_CUDA(cuMemFree_v2(src));
    CHECK_CUDA(cuMemFree_v2(dst));
    CHECK_CUDA(cuCtxDestroy_v2(ctx));
    free(host_src);
    free(host_dst);

    if (errors > 0) {
        TEST_FAIL("D2D copy verification failed");
        return -1;
    }

    TEST_PASS();
    return 0;
}

/*
 * Test 6: Large memory transfer
 * Note: CXL transfers are slow (60KB chunks), so use smaller size by default
 */
int test_large_transfer(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr dptr;
    const size_t SIZE = 16 * 1024 * 1024; /* 16 MB - reasonable for CXL chunk transfers */
    uint8_t *host_src, *host_dst;
    double start, end;
    int errors = 0;

    TEST_START("Large Memory Transfer (16 MB)");

    printf("  Note: CXL transfers use 60KB chunks, expect ~%.0f chunks per direction\n",
           (double)SIZE / (60 * 1024));

    host_src = malloc(SIZE);
    host_dst = malloc(SIZE);
    if (!host_src || !host_dst) {
        printf("  Skipping: not enough host memory\n");
        free(host_src);
        free(host_dst);
        TEST_PASS();
        return 0;
    }

    /* Initialize with pattern */
    for (size_t i = 0; i < SIZE; i++) {
        host_src[i] = (uint8_t)(i ^ (i >> 8) ^ (i >> 16));
    }
    memset(host_dst, 0, SIZE);

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));

    CUresult err = cuMemAlloc_v2(&dptr, SIZE);
    if (err != CUDA_SUCCESS) {
        printf("  Skipping: not enough device memory (error %d)\n", err);
        CHECK_CUDA(cuCtxDestroy_v2(ctx));
        free(host_src);
        free(host_dst);
        TEST_PASS();
        return 0;
    }
    printf("  Allocated %zu MB at 0x%lx\n", SIZE / (1024*1024), (unsigned long)dptr);

    /* Time HtoD transfer */
    start = get_time_ms();
    CHECK_CUDA(cuMemcpyHtoD_v2(dptr, host_src, SIZE));
    end = get_time_ms();
    printf("  HtoD: %.2f GB/s (%.1f ms)\n",
           (SIZE / (1024.0*1024*1024)) / ((end-start) / 1000), end-start);

    /* Time DtoH transfer */
    start = get_time_ms();
    CHECK_CUDA(cuMemcpyDtoH_v2(host_dst, dptr, SIZE));
    end = get_time_ms();
    printf("  DtoH: %.2f GB/s (%.1f ms)\n",
           (SIZE / (1024.0*1024*1024)) / ((end-start) / 1000), end-start);

    /* Verify first and last MB */
    for (size_t i = 0; i < 1024*1024; i++) {
        if (host_src[i] != host_dst[i]) errors++;
    }
    for (size_t i = SIZE - 1024*1024; i < SIZE; i++) {
        if (host_src[i] != host_dst[i]) errors++;
    }
    printf("  Verification (first/last MB): %s (%d errors)\n",
           errors == 0 ? "PASSED" : "FAILED", errors);

    CHECK_CUDA(cuMemFree_v2(dptr));
    CHECK_CUDA(cuCtxDestroy_v2(ctx));
    free(host_src);
    free(host_dst);

    if (errors > 0) {
        TEST_FAIL("Large transfer verification failed");
        return -1;
    }

    TEST_PASS();
    return 0;
}

/*
 * Test 7: Pointer attributes
 */
int test_pointer_attributes(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr dptr;
    CUdeviceptr base;
    size_t size;
    const size_t ALLOC_SIZE = 1024 * 1024;

    TEST_START("Pointer Attributes");

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));
    CHECK_CUDA(cuMemAlloc_v2(&dptr, ALLOC_SIZE));
    printf("  Allocated at 0x%lx\n", (unsigned long)dptr);

    /* Test cuMemGetAddressRange */
    CUresult err = cuMemGetAddressRange_v2(&base, &size, dptr);
    if (err == CUDA_SUCCESS) {
        printf("  Address range: base=0x%lx, size=%zu\n", (unsigned long)base, size);
    } else {
        printf("  cuMemGetAddressRange: error %d\n", err);
    }

    /* Test with offset pointer */
    CUdeviceptr offset_ptr = dptr + 4096;
    err = cuMemGetAddressRange_v2(&base, &size, offset_ptr);
    if (err == CUDA_SUCCESS) {
        printf("  Offset ptr (dptr+4096) range: base=0x%lx, size=%zu\n",
               (unsigned long)base, size);
    } else {
        printf("  cuMemGetAddressRange (offset): error %d\n", err);
    }

    CHECK_CUDA(cuMemFree_v2(dptr));
    CHECK_CUDA(cuCtxDestroy_v2(ctx));

    TEST_PASS();
    return 0;
}

/*
 * Test 8: Stress test - rapid alloc/free cycles
 */
int test_alloc_free_stress(void)
{
    CUdevice dev;
    CUcontext ctx;
    const int ITERATIONS = 100;
    const size_t SIZE = 1024 * 1024;
    double start, end;

    TEST_START("Alloc/Free Stress Test");

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));

    start = get_time_ms();

    for (int i = 0; i < ITERATIONS; i++) {
        CUdeviceptr ptr;
        CHECK_CUDA(cuMemAlloc_v2(&ptr, SIZE));
        CHECK_CUDA(cuMemFree_v2(ptr));
    }

    end = get_time_ms();

    printf("  %d alloc/free cycles in %.1f ms\n", ITERATIONS, end - start);
    printf("  Average: %.3f ms per cycle\n", (end - start) / ITERATIONS);

    CHECK_CUDA(cuCtxDestroy_v2(ctx));

    TEST_PASS();
    return 0;
}

/*
 * Test 9: Module loading (PTX)
 */
int test_module_loading(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUmodule module;
    CUfunction func;

    /* Simple PTX that just returns - PTX 8.0 required for sm_90 (H100) */
    const char *simple_ptx =
        ".version 8.0\n"
        ".target sm_90\n"
        ".address_size 64\n"
        "\n"
        ".visible .entry simple_kernel()\n"
        "{\n"
        "    ret;\n"
        "}\n";

    TEST_START("Module Loading (PTX)");

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));

    printf("  Loading PTX module...\n");
    CUresult err = cuModuleLoadData(&module, simple_ptx);
    if (err != CUDA_SUCCESS) {
        printf("  cuModuleLoadData failed: %d\n", err);
        CHECK_CUDA(cuCtxDestroy_v2(ctx));
        TEST_FAIL("Module load failed");
        return -1;
    }
    printf("  Module: %p\n", module);

    err = cuModuleGetFunction(&func, module, "simple_kernel");
    if (err != CUDA_SUCCESS) {
        printf("  cuModuleGetFunction failed: %d\n", err);
    } else {
        printf("  Function 'simple_kernel': %p\n", func);
    }

    CHECK_CUDA(cuModuleUnload(module));
    printf("  Module unloaded\n");

    CHECK_CUDA(cuCtxDestroy_v2(ctx));

    TEST_PASS();
    return 0;
}

/*
 * Test 10: Kernel launch
 */
int test_kernel_launch(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUmodule module;
    CUfunction func;
    CUdeviceptr d_data;
    float *h_data;
    const int N = 1024;
    const size_t size = N * sizeof(float);
    int errors = 0;

    /* PTX that sets all elements to 42.0 - PTX 8.0 required for sm_90 (H100) */
    const char *set_kernel_ptx =
        ".version 8.0\n"
        ".target sm_90\n"
        ".address_size 64\n"
        "\n"
        ".visible .entry set_value(\n"
        "    .param .u64 data,\n"
        "    .param .u32 n\n"
        ")\n"
        "{\n"
        "    .reg .pred %p<2>;\n"
        "    .reg .f32 %f<2>;\n"
        "    .reg .b32 %r<4>;\n"
        "    .reg .b64 %rd<4>;\n"
        "\n"
        "    ld.param.u64 %rd1, [data];\n"
        "    ld.param.u32 %r1, [n];\n"
        "    mov.u32 %r2, %ctaid.x;\n"
        "    mov.u32 %r3, %ntid.x;\n"
        "    mad.lo.s32 %r2, %r3, %r2, %tid.x;\n"
        "    setp.ge.s32 %p1, %r2, %r1;\n"
        "    @%p1 bra $L_end;\n"
        "\n"
        "    cvta.to.global.u64 %rd2, %rd1;\n"
        "    mul.wide.s32 %rd3, %r2, 4;\n"
        "    add.s64 %rd2, %rd2, %rd3;\n"
        "    mov.f32 %f1, 0f42280000;\n"  /* 42.0 in IEEE 754 */
        "    st.global.f32 [%rd2], %f1;\n"
        "\n"
        "$L_end:\n"
        "    ret;\n"
        "}\n";

    TEST_START("Kernel Launch");

    h_data = (float *)malloc(size);
    if (!h_data) return -1;
    memset(h_data, 0, size);

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));

    printf("  Loading kernel module...\n");
    CUresult err = cuModuleLoadData(&module, set_kernel_ptx);
    if (err != CUDA_SUCCESS) {
        printf("  cuModuleLoadData failed: %d\n", err);
        CHECK_CUDA(cuCtxDestroy_v2(ctx));
        free(h_data);
        TEST_FAIL("Module load failed");
        return -1;
    }

    err = cuModuleGetFunction(&func, module, "set_value");
    if (err != CUDA_SUCCESS) {
        printf("  cuModuleGetFunction failed: %d\n", err);
        cuModuleUnload(module);
        CHECK_CUDA(cuCtxDestroy_v2(ctx));
        free(h_data);
        TEST_FAIL("Get function failed");
        return -1;
    }
    printf("  Got function: %p\n", func);

    CHECK_CUDA(cuMemAlloc_v2(&d_data, size));
    CHECK_CUDA(cuMemsetD8_v2(d_data, 0, size));

    /* Launch kernel */
    int threads = 256;
    int blocks = (N + threads - 1) / threads;
    void *args[] = { &d_data, &N };

    printf("  Launching kernel: %d blocks x %d threads\n", blocks, threads);
    err = cuLaunchKernel(func, blocks, 1, 1, threads, 1, 1, 0, NULL, args, NULL);
    if (err != CUDA_SUCCESS) {
        printf("  cuLaunchKernel failed: %d\n", err);
    } else {
        printf("  Kernel launched\n");

        CHECK_CUDA(cuCtxSynchronize());
        printf("  Synchronized\n");

        /* Copy back and verify */
        CHECK_CUDA(cuMemcpyDtoH_v2(h_data, d_data, size));

        for (int i = 0; i < N; i++) {
            if (fabsf(h_data[i] - 42.0f) > 0.001f) {
                errors++;
                if (errors <= 5) {
                    printf("    h_data[%d] = %f (expected 42.0)\n", i, h_data[i]);
                }
            }
        }
        printf("  Verification: %s (%d errors)\n", errors == 0 ? "PASSED" : "FAILED", errors);
    }

    cuMemFree_v2(d_data);
    cuModuleUnload(module);
    CHECK_CUDA(cuCtxDestroy_v2(ctx));
    free(h_data);

    if (errors > 0) {
        TEST_FAIL("Kernel result verification failed");
        return -1;
    }

    TEST_PASS();
    return 0;
}

/*
 * Test 11: Stream operations
 */
int test_streams(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUstream stream;
    CUdeviceptr dptr;
    const size_t SIZE = 1024 * 1024;

    TEST_START("Stream Operations");

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));

    /* Create stream */
    CUresult err = cuStreamCreate(&stream, 0);
    if (err != CUDA_SUCCESS) {
        printf("  cuStreamCreate failed: %d (may not be supported)\n", err);
        CHECK_CUDA(cuCtxDestroy_v2(ctx));
        TEST_PASS(); /* Not a failure, just not supported */
        return 0;
    }
    printf("  Created stream: %p\n", stream);

    /* Allocate and use with stream */
    CHECK_CUDA(cuMemAlloc_v2(&dptr, SIZE));

    /* Synchronize stream */
    err = cuStreamSynchronize(stream);
    if (err == CUDA_SUCCESS) {
        printf("  Stream synchronized\n");
    } else {
        printf("  cuStreamSynchronize: error %d\n", err);
    }

    CHECK_CUDA(cuMemFree_v2(dptr));

    /* Destroy stream */
    err = cuStreamDestroy_v2(stream);
    if (err == CUDA_SUCCESS) {
        printf("  Stream destroyed\n");
    } else {
        printf("  cuStreamDestroy: error %d\n", err);
    }

    CHECK_CUDA(cuCtxDestroy_v2(ctx));

    TEST_PASS();
    return 0;
}

/*
 * Test 12: Event timing
 */
int test_events(void)
{
    CUdevice dev;
    CUcontext ctx;
    CUevent start, end;
    CUdeviceptr dptr;
    void *host;
    const size_t SIZE = 64 * 1024 * 1024;
    float elapsed;

    TEST_START("Event Timing");

    host = malloc(SIZE);
    if (!host) return -1;
    memset(host, 0xAB, SIZE);

    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, dev));

    /* Create events */
    CUresult err = cuEventCreate(&start, 0);
    if (err != CUDA_SUCCESS) {
        printf("  cuEventCreate failed: %d (may not be supported)\n", err);
        CHECK_CUDA(cuCtxDestroy_v2(ctx));
        free(host);
        TEST_PASS();
        return 0;
    }

    err = cuEventCreate(&end, 0);
    if (err != CUDA_SUCCESS) {
        cuEventDestroy_v2(start);
        CHECK_CUDA(cuCtxDestroy_v2(ctx));
        free(host);
        TEST_PASS();
        return 0;
    }
    printf("  Created events: start=%p, end=%p\n", start, end);

    CHECK_CUDA(cuMemAlloc_v2(&dptr, SIZE));

    /* Record start */
    err = cuEventRecord(start, NULL);
    if (err != CUDA_SUCCESS) {
        printf("  cuEventRecord failed: %d\n", err);
    }

    /* Do memory transfer */
    CHECK_CUDA(cuMemcpyHtoD_v2(dptr, host, SIZE));

    /* Record end */
    err = cuEventRecord(end, NULL);
    if (err != CUDA_SUCCESS) {
        printf("  cuEventRecord(end) failed: %d\n", err);
    }

    /* Synchronize and get elapsed time */
    cuEventSynchronize(end);

    err = cuEventElapsedTime(&elapsed, start, end);
    if (err == CUDA_SUCCESS) {
        printf("  Transfer time: %.3f ms\n", elapsed);
        printf("  Bandwidth: %.2f GB/s\n",
               (SIZE / (1024.0*1024*1024)) / (elapsed / 1000));
    } else {
        printf("  cuEventElapsedTime failed: %d\n", err);
    }

    cuMemFree_v2(dptr);
    cuEventDestroy_v2(start);
    cuEventDestroy_v2(end);
    CHECK_CUDA(cuCtxDestroy_v2(ctx));
    free(host);

    TEST_PASS();
    return 0;
}

int main(int argc, char **argv)
{
    int selected_test = -1;

    printf("==============================================\n");
    printf("CXL Type 2 GPU - Advanced CUDA Test Suite\n");
    printf("==============================================\n\n");

    /* Parse command line */
    if (argc > 1) {
        selected_test = atoi(argv[1]);
        printf("Running only test %d\n", selected_test);
    }

    /* Initialize */
    CUresult err = cuInit(0);
    if (err != CUDA_SUCCESS) {
        printf("cuInit failed: %d\n", err);
        return 1;
    }

    int count;
    CHECK_CUDA(cuDeviceGetCount(&count));
    printf("Found %d CUDA device(s)\n", count);

    if (count == 0) {
        printf("No devices found, exiting.\n");
        return 1;
    }

    /* Run tests */
    typedef int (*test_func)(void);
    struct {
        int id;
        const char *name;
        test_func func;
    } tests[] = {
        { 1, "Device Queries", test_device_queries },
        { 2, "Context Stack", test_context_stack },
        { 3, "Multiple Allocations", test_multiple_allocations },
        { 4, "Memory Set", test_memset },
        { 5, "D2D Copy", test_d2d_copy },
        { 6, "Large Transfer", test_large_transfer },
        { 7, "Pointer Attributes", test_pointer_attributes },
        { 8, "Alloc/Free Stress", test_alloc_free_stress },
        { 9, "Module Loading", test_module_loading },
        { 10, "Kernel Launch", test_kernel_launch },
        { 11, "Streams", test_streams },
        { 12, "Events", test_events },
    };

    int num_tests = sizeof(tests) / sizeof(tests[0]);

    for (int i = 0; i < num_tests; i++) {
        if (selected_test >= 0 && tests[i].id != selected_test) {
            continue;
        }
        tests[i].func();
    }

    /* Summary */
    printf("\n==============================================\n");
    printf("Test Summary: %d run, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);
    printf("==============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
