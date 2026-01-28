/*
 * CXL P2P DMA Test Program
 * Tests P2P DMA transfers between GPU (Type 2) and CXL memory (Type 3)
 *
 * Usage:
 *   LD_LIBRARY_PATH=. ./p2p_test [test_number]
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "cxl_gpu_cmd.h"

/* CUDA types and functions (from libcuda.so) */
typedef int CUresult;
typedef int CUdevice;
typedef void* CUcontext;
typedef uint64_t CUdeviceptr;

#define CUDA_SUCCESS 0

extern CUresult cuInit(unsigned int Flags);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuCtxCreate_v2(CUcontext *pctx, unsigned int flags, CUdevice dev);
extern CUresult cuCtxDestroy_v2(CUcontext ctx);
extern CUresult cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize);
extern CUresult cuMemFree_v2(CUdeviceptr dptr);
extern CUresult cuMemcpyHtoD_v2(CUdeviceptr dstDevice, const void *srcHost, size_t byteCount);
extern CUresult cuMemcpyDtoH_v2(void *dstHost, CUdeviceptr srcDevice, size_t byteCount);
extern CUresult cuCtxSynchronize(void);

/* P2P DMA functions (from libcuda.so) */
typedef struct {
    uint32_t peer_id;
    uint32_t peer_type;
    uint64_t mem_size;
    int coherent;
} CXLPeerInfo;

extern int cxl_p2p_discover_peers(int *num_peers);
extern int cxl_p2p_get_peer_info(uint32_t peer_id, CXLPeerInfo *info);
extern int cxl_p2p_gpu_to_mem(uint32_t t3_peer_id, uint64_t gpu_offset,
                              uint64_t mem_offset, uint64_t size);
extern int cxl_p2p_mem_to_gpu(uint32_t t3_peer_id, uint64_t mem_offset,
                              uint64_t gpu_offset, uint64_t size);
extern int cxl_p2p_mem_to_mem(uint32_t src_peer_id, uint32_t dst_peer_id,
                              uint64_t src_offset, uint64_t dst_offset, uint64_t size);
extern int cxl_p2p_sync(void);
extern int cxl_p2p_get_status(int *num_peers, uint64_t *transfers_completed,
                              uint64_t *bytes_transferred);

/* Test configuration */
#define TEST_SIZE_SMALL     (64 * 1024)      /* 64KB */
#define TEST_SIZE_MEDIUM    (1024 * 1024)    /* 1MB */
#define TEST_SIZE_LARGE     (16 * 1024 * 1024) /* 16MB */

static CUcontext g_ctx = NULL;
static int g_num_peers = 0;
static uint32_t g_type3_peer_id = 0;

/* Test helpers */
#define CHECK_CUDA(call) do { \
    CUresult _result = (call); \
    if (_result != CUDA_SUCCESS) { \
        printf("CUDA error %d at %s:%d\n", _result, __FILE__, __LINE__); \
        return -1; \
    } \
} while(0)

#define CHECK_P2P(call) do { \
    int _result = (call); \
    if (_result != CUDA_SUCCESS) { \
        printf("P2P error %d at %s:%d\n", _result, __FILE__, __LINE__); \
        return -1; \
    } \
} while(0)

static double get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* Initialize CUDA and P2P */
static int init_cuda_and_p2p(void)
{
    CUdevice dev;

    printf("Initializing CUDA and P2P...\n");

    CHECK_CUDA(cuInit(0));
    CHECK_CUDA(cuDeviceGet(&dev, 0));
    CHECK_CUDA(cuCtxCreate_v2(&g_ctx, 0, dev));

    printf("  CUDA initialized\n");

    /* Discover P2P peers */
    CHECK_P2P(cxl_p2p_discover_peers(&g_num_peers));
    printf("  Discovered %d P2P peers\n", g_num_peers);

    if (g_num_peers < 2) {
        printf("  WARNING: Need at least 2 peers (GPU + Type3) for P2P testing\n");
        printf("  Continuing with limited tests...\n");
    }

    /* Find a Type 3 peer */
    for (uint32_t i = 1; i <= (uint32_t)g_num_peers; i++) {
        CXLPeerInfo info;
        if (cxl_p2p_get_peer_info(i, &info) == CUDA_SUCCESS) {
            printf("  Peer %u: type=%u (%s), size=%lu MB, coherent=%d\n",
                   i, info.peer_type,
                   info.peer_type == CXL_P2P_PEER_TYPE2 ? "GPU" : "Memory",
                   info.mem_size / (1024*1024), info.coherent);

            if (info.peer_type == CXL_P2P_PEER_TYPE3 && g_type3_peer_id == 0) {
                g_type3_peer_id = i;
            }
        }
    }

    if (g_type3_peer_id == 0) {
        printf("  WARNING: No Type 3 peer found for P2P testing\n");
    } else {
        printf("  Using Type 3 peer ID %u for P2P tests\n", g_type3_peer_id);
    }

    return 0;
}

static void cleanup_cuda(void)
{
    if (g_ctx) {
        cuCtxDestroy_v2(g_ctx);
        g_ctx = NULL;
    }
}

/* Test 1: Basic P2P Discovery */
static int test_p2p_discovery(void)
{
    printf("\n=== Test 1: P2P Discovery ===\n");

    int num_peers;
    CHECK_P2P(cxl_p2p_discover_peers(&num_peers));

    printf("  Discovered %d peers\n", num_peers);

    for (uint32_t i = 1; i <= (uint32_t)num_peers; i++) {
        CXLPeerInfo info;
        if (cxl_p2p_get_peer_info(i, &info) == CUDA_SUCCESS) {
            printf("  Peer %u: type=%s, mem=%lu MB, coherent=%s\n",
                   i,
                   info.peer_type == CXL_P2P_PEER_TYPE2 ? "Type2(GPU)" : "Type3(Mem)",
                   info.mem_size / (1024*1024),
                   info.coherent ? "yes" : "no");
        }
    }

    printf("  PASSED\n");
    return 0;
}

/* Test 2: GPU to Type3 Memory Transfer */
static int test_gpu_to_mem(void)
{
    printf("\n=== Test 2: GPU to Type3 Memory Transfer ===\n");

    if (g_type3_peer_id == 0) {
        printf("  SKIPPED (no Type 3 peer)\n");
        return 0;
    }

    CUdeviceptr gpu_buf;
    size_t size = TEST_SIZE_SMALL;
    uint8_t *host_data = malloc(size);

    if (!host_data) {
        printf("  Failed to allocate host memory\n");
        return -1;
    }

    /* Fill host data with pattern */
    for (size_t i = 0; i < size; i++) {
        host_data[i] = (uint8_t)(i & 0xFF);
    }

    /* Allocate GPU memory and copy data */
    CHECK_CUDA(cuMemAlloc_v2(&gpu_buf, size));
    CHECK_CUDA(cuMemcpyHtoD_v2(gpu_buf, host_data, size));
    CHECK_CUDA(cuCtxSynchronize());

    printf("  GPU buffer allocated at 0x%lx\n", (unsigned long)gpu_buf);

    /* Transfer GPU -> Type3 */
    double start = get_time_ms();
    CHECK_P2P(cxl_p2p_gpu_to_mem(g_type3_peer_id, gpu_buf, 0, size));
    CHECK_P2P(cxl_p2p_sync());
    double elapsed = get_time_ms() - start;

    printf("  Transferred %zu bytes GPU->Type3 in %.2f ms (%.2f MB/s)\n",
           size, elapsed, (size / (1024.0 * 1024.0)) / (elapsed / 1000.0));

    CHECK_CUDA(cuMemFree_v2(gpu_buf));
    free(host_data);

    printf("  PASSED\n");
    return 0;
}

/* Test 3: Type3 Memory to GPU Transfer */
static int test_mem_to_gpu(void)
{
    printf("\n=== Test 3: Type3 Memory to GPU Transfer ===\n");

    if (g_type3_peer_id == 0) {
        printf("  SKIPPED (no Type 3 peer)\n");
        return 0;
    }

    CUdeviceptr gpu_buf;
    size_t size = TEST_SIZE_SMALL;
    uint8_t *host_verify = malloc(size);

    if (!host_verify) {
        printf("  Failed to allocate host memory\n");
        return -1;
    }

    /* Allocate GPU memory */
    CHECK_CUDA(cuMemAlloc_v2(&gpu_buf, size));

    /* Transfer Type3 -> GPU (assumes data was written in previous test) */
    double start = get_time_ms();
    CHECK_P2P(cxl_p2p_mem_to_gpu(g_type3_peer_id, 0, gpu_buf, size));
    CHECK_P2P(cxl_p2p_sync());
    double elapsed = get_time_ms() - start;

    printf("  Transferred %zu bytes Type3->GPU in %.2f ms (%.2f MB/s)\n",
           size, elapsed, (size / (1024.0 * 1024.0)) / (elapsed / 1000.0));

    /* Verify by copying back to host */
    CHECK_CUDA(cuMemcpyDtoH_v2(host_verify, gpu_buf, size));

    int errors = 0;
    for (size_t i = 0; i < size && errors < 10; i++) {
        if (host_verify[i] != (uint8_t)(i & 0xFF)) {
            printf("  Data mismatch at offset %zu: expected 0x%02x, got 0x%02x\n",
                   i, (uint8_t)(i & 0xFF), host_verify[i]);
            errors++;
        }
    }

    CHECK_CUDA(cuMemFree_v2(gpu_buf));
    free(host_verify);

    if (errors > 0) {
        printf("  FAILED (%d errors)\n", errors);
        return -1;
    }

    printf("  PASSED\n");
    return 0;
}

/* Test 4: Round-trip P2P Transfer */
static int test_roundtrip(void)
{
    printf("\n=== Test 4: Round-trip P2P Transfer ===\n");

    if (g_type3_peer_id == 0) {
        printf("  SKIPPED (no Type 3 peer)\n");
        return 0;
    }

    CUdeviceptr gpu_buf;
    size_t size = TEST_SIZE_MEDIUM;
    uint8_t *host_original = malloc(size);
    uint8_t *host_verify = malloc(size);

    if (!host_original || !host_verify) {
        printf("  Failed to allocate host memory\n");
        free(host_original);
        free(host_verify);
        return -1;
    }

    /* Fill with random data */
    srand(42);
    for (size_t i = 0; i < size; i++) {
        host_original[i] = (uint8_t)(rand() & 0xFF);
    }

    /* Allocate GPU memory and copy original data */
    CHECK_CUDA(cuMemAlloc_v2(&gpu_buf, size));
    CHECK_CUDA(cuMemcpyHtoD_v2(gpu_buf, host_original, size));
    CHECK_CUDA(cuCtxSynchronize());

    /* GPU -> Type3 -> GPU */
    double start = get_time_ms();
    CHECK_P2P(cxl_p2p_gpu_to_mem(g_type3_peer_id, gpu_buf, 0x100000, size));
    CHECK_P2P(cxl_p2p_sync());
    CHECK_P2P(cxl_p2p_mem_to_gpu(g_type3_peer_id, 0x100000, gpu_buf, size));
    CHECK_P2P(cxl_p2p_sync());
    double elapsed = get_time_ms() - start;

    printf("  Round-trip %zu bytes in %.2f ms\n", size, elapsed);

    /* Verify data integrity */
    CHECK_CUDA(cuMemcpyDtoH_v2(host_verify, gpu_buf, size));

    int errors = 0;
    for (size_t i = 0; i < size; i++) {
        if (host_verify[i] != host_original[i]) {
            if (errors < 10) {
                printf("  Mismatch at %zu: expected 0x%02x, got 0x%02x\n",
                       i, host_original[i], host_verify[i]);
            }
            errors++;
        }
    }

    CHECK_CUDA(cuMemFree_v2(gpu_buf));
    free(host_original);
    free(host_verify);

    if (errors > 0) {
        printf("  FAILED (%d errors)\n", errors);
        return -1;
    }

    printf("  PASSED\n");
    return 0;
}

/* Test 5: Large Transfer Performance */
static int test_large_transfer(void)
{
    printf("\n=== Test 5: Large Transfer Performance ===\n");

    if (g_type3_peer_id == 0) {
        printf("  SKIPPED (no Type 3 peer)\n");
        return 0;
    }

    CUdeviceptr gpu_buf;
    size_t size = TEST_SIZE_LARGE;

    /* Allocate GPU memory */
    CHECK_CUDA(cuMemAlloc_v2(&gpu_buf, size));

    /* Measure GPU -> Type3 bandwidth */
    double start = get_time_ms();
    CHECK_P2P(cxl_p2p_gpu_to_mem(g_type3_peer_id, gpu_buf, 0, size));
    CHECK_P2P(cxl_p2p_sync());
    double elapsed_g2m = get_time_ms() - start;

    double bw_g2m = (size / (1024.0 * 1024.0)) / (elapsed_g2m / 1000.0);
    printf("  GPU->Type3: %zu bytes in %.2f ms = %.2f MB/s\n",
           size, elapsed_g2m, bw_g2m);

    /* Measure Type3 -> GPU bandwidth */
    start = get_time_ms();
    CHECK_P2P(cxl_p2p_mem_to_gpu(g_type3_peer_id, 0, gpu_buf, size));
    CHECK_P2P(cxl_p2p_sync());
    double elapsed_m2g = get_time_ms() - start;

    double bw_m2g = (size / (1024.0 * 1024.0)) / (elapsed_m2g / 1000.0);
    printf("  Type3->GPU: %zu bytes in %.2f ms = %.2f MB/s\n",
           size, elapsed_m2g, bw_m2g);

    CHECK_CUDA(cuMemFree_v2(gpu_buf));

    printf("  PASSED\n");
    return 0;
}

/* Test 6: P2P Status and Statistics */
static int test_p2p_status(void)
{
    printf("\n=== Test 6: P2P Status and Statistics ===\n");

    int num_peers;
    uint64_t transfers_completed;
    uint64_t bytes_transferred;

    CHECK_P2P(cxl_p2p_get_status(&num_peers, &transfers_completed, &bytes_transferred));

    printf("  Number of peers: %d\n", num_peers);
    printf("  Transfers completed: %lu\n", (unsigned long)transfers_completed);
    printf("  Bytes transferred: %lu (%.2f MB)\n",
           (unsigned long)bytes_transferred,
           bytes_transferred / (1024.0 * 1024.0));

    printf("  PASSED\n");
    return 0;
}

/* Test list */
typedef struct {
    const char *name;
    int (*func)(void);
} TestEntry;

static TestEntry tests[] = {
    { "P2P Discovery", test_p2p_discovery },
    { "GPU to Type3 Memory", test_gpu_to_mem },
    { "Type3 Memory to GPU", test_mem_to_gpu },
    { "Round-trip Transfer", test_roundtrip },
    { "Large Transfer Performance", test_large_transfer },
    { "P2P Status", test_p2p_status },
};

#define NUM_TESTS (sizeof(tests) / sizeof(tests[0]))

int main(int argc, char *argv[])
{
    int test_num = -1;
    int passed = 0, failed = 0, skipped = 0;

    printf("CXL P2P DMA Test Suite\n");
    printf("======================\n");

    if (argc > 1) {
        test_num = atoi(argv[1]);
        printf("Running test %d only\n", test_num);
    }

    /* Initialize */
    if (init_cuda_and_p2p() != 0) {
        printf("Failed to initialize CUDA and P2P\n");
        return 1;
    }

    /* Run tests */
    for (int i = 0; i < (int)NUM_TESTS; i++) {
        if (test_num >= 0 && test_num != i + 1) {
            continue;
        }

        int result = tests[i].func();
        if (result == 0) {
            passed++;
        } else if (result > 0) {
            skipped++;
        } else {
            failed++;
        }
    }

    /* Print summary */
    printf("\n======================\n");
    printf("Test Summary: %d passed, %d failed, %d skipped\n",
           passed, failed, skipped);

    /* Print final P2P statistics */
    printf("\nFinal P2P Statistics:\n");
    test_p2p_status();

    cleanup_cuda();

    return failed > 0 ? 1 : 0;
}
