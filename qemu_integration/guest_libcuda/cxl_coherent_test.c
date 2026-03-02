/*
 * CXL Coherent Shared Memory Test
 * Tests basic coherent alloc/free and read/write verification
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "cxl_gpu_cmd.h"

/* CUDA types (minimal) */
typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef uint64_t CUdeviceptr;

#define CUDA_SUCCESS 0

/* External APIs from libcuda */
extern CUresult cuInit(unsigned int flags);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev);
extern CUresult cuMemcpyDtoH_v2(void *dst, CUdeviceptr src, size_t size);

extern int cxlCoherentAlloc(uint64_t size, void **host_ptr);
extern int cxlCoherentFree(void *host_ptr);
extern uint64_t cxlHostToDevice(void *host_ptr);
extern void *cxlDeviceToHost(uint64_t dev_offset);
extern int cxlCoherentFence(void);

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("  [TEST] %s... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

static void test_basic_alloc_free(void)
{
    TEST("Basic coherent alloc/free");

    void *ptr = NULL;
    int ret = cxlCoherentAlloc(4096, &ptr);
    if (ret != CUDA_SUCCESS) {
        FAIL("alloc failed");
        return;
    }
    if (!ptr) {
        FAIL("got NULL pointer");
        return;
    }

    ret = cxlCoherentFree(ptr);
    if (ret != CUDA_SUCCESS) {
        FAIL("free failed");
        return;
    }

    PASS();
}

static void test_multiple_allocs(void)
{
    TEST("Multiple coherent allocations");

    void *ptrs[8];
    int i;

    for (i = 0; i < 8; i++) {
        int ret = cxlCoherentAlloc(4096 * (i + 1), &ptrs[i]);
        if (ret != CUDA_SUCCESS || !ptrs[i]) {
            FAIL("alloc failed");
            /* Free what we allocated */
            for (int j = 0; j < i; j++) {
                cxlCoherentFree(ptrs[j]);
            }
            return;
        }
    }

    /* Free in reverse order */
    for (i = 7; i >= 0; i--) {
        int ret = cxlCoherentFree(ptrs[i]);
        if (ret != CUDA_SUCCESS) {
            FAIL("free failed");
            return;
        }
    }

    PASS();
}

static void test_cpu_write_read(void)
{
    TEST("CPU write and readback through coherent memory");

    void *ptr = NULL;
    int ret = cxlCoherentAlloc(4096, &ptr);
    if (ret != CUDA_SUCCESS || !ptr) {
        FAIL("alloc failed");
        return;
    }

    /* Write a pattern */
    uint32_t *data = (uint32_t *)ptr;
    for (int i = 0; i < 1024; i++) {
        data[i] = 0xDEAD0000 | i;
    }

    /* Fence to ensure visibility */
    cxlCoherentFence();

    /* Read back and verify */
    int errors = 0;
    for (int i = 0; i < 1024; i++) {
        if (data[i] != (0xDEAD0000 | (uint32_t)i)) {
            errors++;
        }
    }

    cxlCoherentFree(ptr);

    if (errors > 0) {
        FAIL("data mismatch");
    } else {
        PASS();
    }
}

static void test_pointer_translation(void)
{
    TEST("Pointer translation roundtrip");

    void *ptr = NULL;
    int ret = cxlCoherentAlloc(4096, &ptr);
    if (ret != CUDA_SUCCESS || !ptr) {
        FAIL("alloc failed");
        return;
    }

    /* host -> device -> host roundtrip */
    uint64_t dev_offset = cxlHostToDevice(ptr);
    void *roundtrip_ptr = cxlDeviceToHost(dev_offset);

    if (roundtrip_ptr != ptr) {
        FAIL("pointer roundtrip mismatch");
        cxlCoherentFree(ptr);
        return;
    }

    cxlCoherentFree(ptr);
    PASS();
}

static void test_large_alloc(void)
{
    TEST("Large coherent allocation (1MB)");

    void *ptr = NULL;
    int ret = cxlCoherentAlloc(1024 * 1024, &ptr);
    if (ret != CUDA_SUCCESS || !ptr) {
        FAIL("alloc failed");
        return;
    }

    /* Write pattern across full range */
    uint64_t *data = (uint64_t *)ptr;
    size_t count = (1024 * 1024) / sizeof(uint64_t);
    for (size_t i = 0; i < count; i++) {
        data[i] = i * 0x0123456789ABCDEFULL;
    }

    cxlCoherentFence();

    /* Verify */
    int errors = 0;
    for (size_t i = 0; i < count; i++) {
        if (data[i] != i * 0x0123456789ABCDEFULL) {
            errors++;
        }
    }

    cxlCoherentFree(ptr);

    if (errors > 0) {
        FAIL("data mismatch in large alloc");
    } else {
        PASS();
    }
}

static void test_double_free_rejected(void)
{
    TEST("Double free rejected");

    void *ptr = NULL;
    int ret = cxlCoherentAlloc(4096, &ptr);
    if (ret != CUDA_SUCCESS || !ptr) {
        FAIL("alloc failed");
        return;
    }

    ret = cxlCoherentFree(ptr);
    if (ret != CUDA_SUCCESS) {
        FAIL("first free failed");
        return;
    }

    /* Second free should fail */
    ret = cxlCoherentFree(ptr);
    if (ret == CUDA_SUCCESS) {
        FAIL("double free was accepted (should fail)");
        return;
    }

    PASS();
}

int main(void)
{
    printf("=== CXL Coherent Shared Memory Test ===\n\n");

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

    printf("Running coherent memory tests:\n");
    test_basic_alloc_free();
    test_multiple_allocs();
    test_cpu_write_read();
    test_pointer_translation();
    test_large_alloc();
    test_double_free_rejected();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
