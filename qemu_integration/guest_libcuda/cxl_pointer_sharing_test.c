/*
 * CXL Pointer Sharing Test
 * Linked-list and graph traversal with shared CPU-GPU pointers
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

/* External APIs */
extern CUresult cuInit(unsigned int flags);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev);
extern CUresult cuMemAlloc_v2(CUdeviceptr *dptr, size_t size);
extern CUresult cuMemFree_v2(CUdeviceptr dptr);
extern CUresult cuMemcpyHtoD_v2(CUdeviceptr dst, const void *src, size_t size);
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

/*
 * Offset-based linked list node. Both CPU and GPU use the same offset
 * within BAR4 to reference nodes, enabling true pointer sharing.
 */
typedef struct CohListNode {
    uint64_t next_offset;   /* 0 = end of list, otherwise BAR4 offset */
    uint64_t value;
} CohListNode;

#define LIST_END 0

static void test_linked_list_build_traverse(void)
{
    TEST("Build and traverse linked list in coherent memory");

    const int NUM_NODES = 100;
    size_t total_size = sizeof(CohListNode) * NUM_NODES;

    void *pool = NULL;
    int ret = cxlCoherentAlloc(total_size, &pool);
    if (ret != CUDA_SUCCESS || !pool) {
        FAIL("coherent alloc failed");
        return;
    }

    CohListNode *nodes = (CohListNode *)pool;
    uint64_t base_offset = cxlHostToDevice(pool);

    /* Build linked list: node[0] -> node[1] -> ... -> node[N-1] */
    for (int i = 0; i < NUM_NODES; i++) {
        nodes[i].value = (uint64_t)(i * 42 + 7);
        if (i < NUM_NODES - 1) {
            nodes[i].next_offset = base_offset +
                                   (uint64_t)(i + 1) * sizeof(CohListNode);
        } else {
            nodes[i].next_offset = LIST_END;
        }
    }

    /* Fence to ensure GPU can see the writes */
    cxlCoherentFence();

    /* Traverse the list using offsets (simulating GPU-side traversal) */
    uint64_t cur_offset = base_offset;
    int count = 0;
    int errors = 0;

    while (cur_offset != LIST_END && count < NUM_NODES + 1) {
        CohListNode *node = (CohListNode *)cxlDeviceToHost(cur_offset);
        if (!node) {
            errors++;
            break;
        }

        uint64_t expected = (uint64_t)(count * 42 + 7);
        if (node->value != expected) {
            errors++;
        }

        cur_offset = node->next_offset;
        count++;
    }

    cxlCoherentFree(pool);

    if (count != NUM_NODES || errors > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "traversed %d/%d nodes, %d errors",
                 count, NUM_NODES, errors);
        FAIL(msg);
    } else {
        PASS();
    }
}

static void test_linked_list_reverse(void)
{
    TEST("Reverse linked list in-place using shared pointers");

    const int NUM_NODES = 50;
    size_t total_size = sizeof(CohListNode) * NUM_NODES;

    void *pool = NULL;
    int ret = cxlCoherentAlloc(total_size, &pool);
    if (ret != CUDA_SUCCESS || !pool) {
        FAIL("coherent alloc failed");
        return;
    }

    CohListNode *nodes = (CohListNode *)pool;
    uint64_t base_offset = cxlHostToDevice(pool);

    /* Build forward list */
    for (int i = 0; i < NUM_NODES; i++) {
        nodes[i].value = (uint64_t)i;
        if (i < NUM_NODES - 1) {
            nodes[i].next_offset = base_offset +
                                   (uint64_t)(i + 1) * sizeof(CohListNode);
        } else {
            nodes[i].next_offset = LIST_END;
        }
    }

    cxlCoherentFence();

    /* Reverse the list in-place */
    uint64_t prev_offset = LIST_END;
    uint64_t cur_offset = base_offset;

    while (cur_offset != LIST_END) {
        CohListNode *cur = (CohListNode *)cxlDeviceToHost(cur_offset);
        uint64_t next_offset = cur->next_offset;
        cur->next_offset = prev_offset;
        prev_offset = cur_offset;
        cur_offset = next_offset;
    }

    cxlCoherentFence();

    /* Traverse reversed list and verify */
    cur_offset = prev_offset; /* New head */
    int count = 0;
    int errors = 0;

    while (cur_offset != LIST_END && count < NUM_NODES + 1) {
        CohListNode *node = (CohListNode *)cxlDeviceToHost(cur_offset);
        uint64_t expected = (uint64_t)(NUM_NODES - 1 - count);
        if (node->value != expected) {
            errors++;
        }
        cur_offset = node->next_offset;
        count++;
    }

    cxlCoherentFree(pool);

    if (count != NUM_NODES || errors > 0) {
        FAIL("reverse traversal mismatch");
    } else {
        PASS();
    }
}

/*
 * Simple graph node for testing graph traversal with shared pointers.
 * Uses offset-based adjacency list.
 */
#define MAX_NEIGHBORS 4

typedef struct CohGraphNode {
    uint64_t id;
    uint64_t neighbor_offsets[MAX_NEIGHBORS];
    uint32_t num_neighbors;
    uint32_t visited;
} CohGraphNode;

static void test_graph_traversal(void)
{
    TEST("Graph traversal with shared pointers (BFS)");

    const int NUM_NODES = 16;
    size_t total_size = sizeof(CohGraphNode) * NUM_NODES;

    void *pool = NULL;
    int ret = cxlCoherentAlloc(total_size, &pool);
    if (ret != CUDA_SUCCESS || !pool) {
        FAIL("coherent alloc failed");
        return;
    }

    CohGraphNode *graph = (CohGraphNode *)pool;
    uint64_t base_offset = cxlHostToDevice(pool);

    /* Build a simple ring + tree graph */
    for (int i = 0; i < NUM_NODES; i++) {
        graph[i].id = (uint64_t)i;
        graph[i].num_neighbors = 0;
        graph[i].visited = 0;
        memset(graph[i].neighbor_offsets, 0, sizeof(graph[i].neighbor_offsets));
    }

    /* Ring: 0->1->2->...->15->0 */
    for (int i = 0; i < NUM_NODES; i++) {
        int next = (i + 1) % NUM_NODES;
        graph[i].neighbor_offsets[graph[i].num_neighbors++] =
            base_offset + (uint64_t)next * sizeof(CohGraphNode);
    }

    cxlCoherentFence();

    /* BFS from node 0 */
    uint64_t queue[32];
    int head = 0, tail = 0;
    int visit_count = 0;

    graph[0].visited = 1;
    queue[tail++] = base_offset;

    while (head < tail && visit_count < NUM_NODES + 1) {
        uint64_t node_offset = queue[head++];
        CohGraphNode *node = (CohGraphNode *)cxlDeviceToHost(node_offset);
        visit_count++;

        for (uint32_t j = 0; j < node->num_neighbors; j++) {
            CohGraphNode *neighbor =
                (CohGraphNode *)cxlDeviceToHost(node->neighbor_offsets[j]);
            if (neighbor && !neighbor->visited) {
                neighbor->visited = 1;
                if (tail < 32) {
                    queue[tail++] = node->neighbor_offsets[j];
                }
            }
        }
    }

    cxlCoherentFree(pool);

    if (visit_count != NUM_NODES) {
        char msg[128];
        snprintf(msg, sizeof(msg), "visited %d/%d nodes", visit_count, NUM_NODES);
        FAIL(msg);
    } else {
        PASS();
    }
}

static void test_copy_vs_coherent_comparison(void)
{
    TEST("Copy-based vs coherent comparison (timing)");

    const int NUM_NODES = 1000;
    size_t node_size = sizeof(CohListNode);
    size_t total_size = node_size * NUM_NODES;

    /* Method 1: Copy-based (traditional cuMemcpy approach) */
    CohListNode *host_list = malloc(total_size);
    if (!host_list) {
        FAIL("malloc failed");
        return;
    }

    /* Build host-side list with sequential indices as "pointers" */
    for (int i = 0; i < NUM_NODES; i++) {
        host_list[i].value = (uint64_t)(i * 13 + 5);
        host_list[i].next_offset = (i < NUM_NODES - 1) ? (uint64_t)(i + 1) : 0;
    }

    struct timespec t1, t2;

    /* Time copy-based traversal */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int count = 0;
    uint64_t idx = 0;
    while (idx < (uint64_t)NUM_NODES && count < NUM_NODES) {
        /* In real copy-based approach, each node access would be a cuMemcpyDtoH */
        volatile uint64_t val = host_list[idx].value;
        (void)val;
        uint64_t next = host_list[idx].next_offset;
        if (next == 0) break;
        idx = next;
        count++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    uint64_t copy_ns = (t2.tv_sec - t1.tv_sec) * 1000000000ULL +
                       (t2.tv_nsec - t1.tv_nsec);

    /* Method 2: Coherent memory */
    void *coh_pool = NULL;
    int ret = cxlCoherentAlloc(total_size, &coh_pool);
    if (ret != CUDA_SUCCESS || !coh_pool) {
        free(host_list);
        FAIL("coherent alloc failed");
        return;
    }

    CohListNode *coh_nodes = (CohListNode *)coh_pool;
    uint64_t base = cxlHostToDevice(coh_pool);

    for (int i = 0; i < NUM_NODES; i++) {
        coh_nodes[i].value = (uint64_t)(i * 13 + 5);
        if (i < NUM_NODES - 1) {
            coh_nodes[i].next_offset = base +
                                       (uint64_t)(i + 1) * sizeof(CohListNode);
        } else {
            coh_nodes[i].next_offset = LIST_END;
        }
    }
    cxlCoherentFence();

    clock_gettime(CLOCK_MONOTONIC, &t1);
    count = 0;
    uint64_t cur_off = base;
    while (cur_off != LIST_END && count < NUM_NODES) {
        CohListNode *node = (CohListNode *)cxlDeviceToHost(cur_off);
        volatile uint64_t val = node->value;
        (void)val;
        cur_off = node->next_offset;
        count++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    uint64_t coh_ns = (t2.tv_sec - t1.tv_sec) * 1000000000ULL +
                      (t2.tv_nsec - t1.tv_nsec);

    printf("\n    Copy-based traversal: %lu ns (%lu ns/node)\n",
           (unsigned long)copy_ns, (unsigned long)(copy_ns / NUM_NODES));
    printf("    Coherent traversal:   %lu ns (%lu ns/node)\n",
           (unsigned long)coh_ns, (unsigned long)(coh_ns / NUM_NODES));

    free(host_list);
    cxlCoherentFree(coh_pool);

    /* Both should complete without error */
    PASS();
}

int main(void)
{
    printf("=== CXL Pointer Sharing Test ===\n\n");

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

    printf("Running pointer sharing tests:\n");
    test_linked_list_build_traverse();
    test_linked_list_reverse();
    test_graph_traversal();
    test_copy_vs_coherent_comparison();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
