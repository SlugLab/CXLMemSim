/*
 * RQ1: Pointer Sharing vs Copy-Based Offload for Pointer-Rich Workloads
 *
 * Experiments:
 *   1. BFS on Erdos-Renyi and Barabasi-Albert graphs
 *      - Pointer sharing via CXL coherent memory
 *      - Copy-based offload via cuMemcpy
 *      - Sweep device_fraction in {0.0, 0.25, 0.5, 0.75, 1.0}
 *   2. Simple concurrent B+ tree benchmark
 *      - Internal nodes in host mem, leaves in coherent device mem
 *      - Sweep branching factor B in {4, 16, 64, 256}
 *   3. Chained hash table
 *      - Buckets in coherent device memory, chain node allocation policies
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#include "cxl_gpu_cmd.h"

/* ------------------------------------------------------------------ */
/* CUDA types                                                         */
/* ------------------------------------------------------------------ */
typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef uint64_t CUdeviceptr;

#define CUDA_SUCCESS 0

/* ------------------------------------------------------------------ */
/* External APIs                                                      */
/* ------------------------------------------------------------------ */
extern CUresult cuInit(unsigned int flags);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev);
extern CUresult cuMemAlloc_v2(CUdeviceptr *dptr, size_t size);
extern CUresult cuMemFree_v2(CUdeviceptr dptr);
extern CUresult cuMemcpyHtoD_v2(CUdeviceptr dst, const void *src, size_t size);
extern CUresult cuMemcpyDtoH_v2(void *dst, CUdeviceptr src, size_t size);

extern int cxlCoherentAlloc(uint64_t size, void **host_ptr);
extern int cxlCoherentFree(void *host_ptr);
extern void *cxlDeviceToHost(uint64_t dev_offset);
extern int cxlCoherentFence(void);
extern int cxlSetBias(void *host_ptr, uint64_t size, int bias_mode);
extern int cxlGetBias(void *host_ptr, int *bias_mode);

typedef struct {
    uint64_t snoop_hits, snoop_misses, coherency_requests, back_invalidations;
    uint64_t writebacks, evictions, bias_flips;
    uint64_t device_bias_hits, host_bias_hits;
    uint64_t upgrades, downgrades, directory_entries;
} CXLCoherencyStats;

extern int cxlGetCoherencyStats(CXLCoherencyStats *stats);
extern int cxlResetCoherencyStats(void);

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */
#define DEFAULT_NUM_NODES   2000
#define AVG_DEGREE          6
#define MAX_NEIGHBORS       6
#define NUM_TRIALS          3
#define BFS_QUEUE_SCALE     4       /* queue = BFS_QUEUE_SCALE * N */

/* Graph node: exactly 64 bytes (one cache line) */
typedef struct GraphNode {
    uint64_t id;                        /*  8 bytes */
    uint32_t visited;                   /*  4 bytes */
    uint32_t num_neighbors;             /*  4 bytes */
    uint64_t neighbor_offsets[6];       /* 48 bytes */
} GraphNode;                            /* = 64 bytes total */

/* ------------------------------------------------------------------ */
/* Utility: timing                                                    */
/* ------------------------------------------------------------------ */
static inline uint64_t ts_to_ns(struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static inline uint64_t elapsed_ns(struct timespec *t0, struct timespec *t1)
{
    return ts_to_ns(t1) - ts_to_ns(t0);
}

/* ------------------------------------------------------------------ */
/* Utility: BAR4 base for offset computation                          */
/* ------------------------------------------------------------------ */
static uint8_t *g_bar4_base;

static void init_bar4_base(void)
{
    /* cxlDeviceToHost(0) returns bar4 + 0, i.e. the BAR4 base pointer */
    g_bar4_base = (uint8_t *)cxlDeviceToHost(0);
}

static inline uint64_t host_to_dev_offset(void *host_ptr)
{
    return (uint64_t)((uint8_t *)host_ptr - g_bar4_base);
}

/* ------------------------------------------------------------------ */
/* Utility: simple PRNG (xorshift64)                                  */
/* ------------------------------------------------------------------ */
static uint64_t rng_state = 0x12345678DEADBEEFULL;

static inline uint64_t xorshift64(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static inline double rng_uniform(void)
{
    return (double)(xorshift64() & 0xFFFFFFFFFFFFULL) / (double)0xFFFFFFFFFFFFULL;
}

static void rng_seed(uint64_t s)
{
    rng_state = s ? s : 1;
}

/* ------------------------------------------------------------------ */
/* Utility: percentile on sorted array                                */
/* ------------------------------------------------------------------ */
static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static void compute_stats(uint64_t *vals, int n,
                           uint64_t *med, uint64_t *p25, uint64_t *p75)
{
    qsort(vals, (size_t)n, sizeof(uint64_t), cmp_u64);
    *p25 = vals[n / 4];
    *med = vals[n / 2];
    *p75 = vals[(3 * n) / 4];
}

/* ------------------------------------------------------------------ */
/* Utility: print coherency stats                                     */
/* ------------------------------------------------------------------ */
static void print_coherency_stats(const char *prefix)
{
    CXLCoherencyStats st;
    if (cxlGetCoherencyStats(&st) != CUDA_SUCCESS) {
        printf("%s  (unable to read coherency stats)\n", prefix);
        return;
    }
    printf("%s  coherency: snoop_hits=%lu snoop_misses=%lu "
           "coh_reqs=%lu back_inv=%lu\n",
           prefix,
           (unsigned long)st.snoop_hits, (unsigned long)st.snoop_misses,
           (unsigned long)st.coherency_requests,
           (unsigned long)st.back_invalidations);
    printf("%s            writebacks=%lu evictions=%lu bias_flips=%lu\n",
           prefix,
           (unsigned long)st.writebacks, (unsigned long)st.evictions,
           (unsigned long)st.bias_flips);
    printf("%s            dev_bias_hits=%lu host_bias_hits=%lu "
           "dir_entries=%lu\n",
           prefix,
           (unsigned long)st.device_bias_hits,
           (unsigned long)st.host_bias_hits,
           (unsigned long)st.directory_entries);
}

/* ================================================================== */
/*   EXPERIMENT 1:  BFS on random graphs                              */
/* ================================================================== */

/* ---- Graph generation helpers ------------------------------------ */

/*
 * Build an Erdos-Renyi graph: each node gets ~avg_degree random neighbors.
 * We sample neighbors directly rather than iterating all O(N^2) pairs.
 */
static void build_erdos_renyi(GraphNode *nodes, int N, uint64_t *offsets)
{
    for (int i = 0; i < N; i++) {
        nodes[i].id = (uint64_t)i;
        nodes[i].visited = 0;
        nodes[i].num_neighbors = 0;
        memset(nodes[i].neighbor_offsets, 0, sizeof(nodes[i].neighbor_offsets));
    }

    /* For each node, sample ~AVG_DEGREE neighbors */
    for (int i = 0; i < N; i++) {
        int target_deg = AVG_DEGREE;
        if (target_deg > MAX_NEIGHBORS)
            target_deg = MAX_NEIGHBORS;
        if (target_deg > N - 1)
            target_deg = N - 1;

        int added = 0;
        int attempts = 0;
        while (added < target_deg && attempts < target_deg * 4) {
            attempts++;
            int j = (int)(xorshift64() % (uint64_t)N);
            if (j == i) continue;

            /* Check for duplicate */
            int dup = 0;
            for (uint32_t k = 0; k < nodes[i].num_neighbors; k++) {
                if (nodes[i].neighbor_offsets[k] == offsets[j]) {
                    dup = 1;
                    break;
                }
            }
            if (dup) continue;

            nodes[i].neighbor_offsets[nodes[i].num_neighbors++] = offsets[j];
            added++;
        }
    }
}

/*
 * Build a Barabasi-Albert (preferential attachment) graph.
 * Start with a small clique of m0 nodes, then attach new nodes with
 * m edges each, where m = min(AVG_DEGREE/2, m0).
 */
static void build_barabasi_albert(GraphNode *nodes, int N, uint64_t *offsets)
{
    int m0 = AVG_DEGREE;
    int m  = AVG_DEGREE / 2;
    if (m < 1) m = 1;
    if (m0 < m) m0 = m;
    if (m0 > N) m0 = N;

    /* degree array for preferential attachment */
    int *degree = calloc((size_t)N, sizeof(int));
    if (!degree) {
        fprintf(stderr, "    [WARN] calloc for degree array failed\n");
        return;
    }

    for (int i = 0; i < N; i++) {
        nodes[i].id = (uint64_t)i;
        nodes[i].visited = 0;
        nodes[i].num_neighbors = 0;
        memset(nodes[i].neighbor_offsets, 0, sizeof(nodes[i].neighbor_offsets));
    }

    /* Initial clique of m0 nodes */
    for (int i = 0; i < m0; i++) {
        for (int j = i + 1; j < m0; j++) {
            if (nodes[i].num_neighbors < MAX_NEIGHBORS) {
                nodes[i].neighbor_offsets[nodes[i].num_neighbors++] = offsets[j];
                degree[i]++;
            }
            if (nodes[j].num_neighbors < MAX_NEIGHBORS) {
                nodes[j].neighbor_offsets[nodes[j].num_neighbors++] = offsets[i];
                degree[j]++;
            }
        }
    }

    int total_degree = 0;
    for (int i = 0; i < m0; i++) total_degree += degree[i];

    /* Attach remaining nodes */
    for (int new_node = m0; new_node < N; new_node++) {
        int added = 0;
        int attempts = 0;
        while (added < m && attempts < m * 20) {
            attempts++;
            /* Choose target proportional to degree */
            int target;
            if (total_degree > 0) {
                double r = rng_uniform() * (double)total_degree;
                double cumul = 0.0;
                target = 0;
                for (int t = 0; t < new_node; t++) {
                    cumul += (double)degree[t];
                    if (cumul >= r) { target = t; break; }
                    target = t;
                }
            } else {
                target = (int)(xorshift64() % (uint64_t)new_node);
            }

            /* Check for duplicate edge */
            int dup = 0;
            for (uint32_t k = 0; k < nodes[new_node].num_neighbors; k++) {
                if (nodes[new_node].neighbor_offsets[k] == offsets[target]) {
                    dup = 1;
                    break;
                }
            }
            if (dup) continue;
            if (nodes[new_node].num_neighbors >= MAX_NEIGHBORS) break;
            if (nodes[target].num_neighbors >= MAX_NEIGHBORS) continue;

            nodes[new_node].neighbor_offsets[nodes[new_node].num_neighbors++] =
                offsets[target];
            nodes[target].neighbor_offsets[nodes[target].num_neighbors++] =
                offsets[new_node];
            degree[new_node]++;
            degree[target]++;
            total_degree += 2;
            added++;
        }
    }

    free(degree);
}

/* ---- BFS implementations ---------------------------------------- */

/*
 * Method A: Pointer-sharing BFS (coherent).
 * All nodes live in coherent memory.  Neighbor offsets are BAR4 device
 * offsets resolved via cxlDeviceToHost().
 */
static int bfs_coherent(uint64_t *node_offsets, int N, int start_id)
{
    /* Reset visited flags */
    for (int i = 0; i < N; i++) {
        GraphNode *nd = (GraphNode *)cxlDeviceToHost(node_offsets[i]);
        if (!nd) return -1;
        nd->visited = 0;
    }
    cxlCoherentFence();

    /* BFS queue (offsets into coherent memory) */
    int qcap = N * BFS_QUEUE_SCALE;
    if (qcap < 256) qcap = 256;
    uint64_t *queue = malloc((size_t)qcap * sizeof(uint64_t));
    if (!queue) return -1;

    int head = 0, tail = 0;
    int visited_count = 0;

    /* Enqueue start */
    GraphNode *start = (GraphNode *)cxlDeviceToHost(node_offsets[start_id]);
    if (!start) { free(queue); return -1; }
    start->visited = 1;
    queue[tail++] = node_offsets[start_id];
    visited_count++;

    while (head < tail) {
        uint64_t cur_off = queue[head++];
        GraphNode *cur = (GraphNode *)cxlDeviceToHost(cur_off);
        if (!cur) continue;

        for (uint32_t j = 0; j < cur->num_neighbors && j < MAX_NEIGHBORS; j++) {
            uint64_t nb_off = cur->neighbor_offsets[j];
            if (nb_off == 0) continue;
            GraphNode *nb = (GraphNode *)cxlDeviceToHost(nb_off);
            if (!nb) continue;
            if (!nb->visited) {
                nb->visited = 1;
                visited_count++;
                if (tail < qcap) {
                    queue[tail++] = nb_off;
                }
            }
        }
    }

    free(queue);
    return visited_count;
}

/*
 * Method B: Copy-based BFS.
 * Graph lives in host memory.  Each BFS level serializes the frontier
 * to device via cuMemcpyHtoD, then copies results back.
 */
static int bfs_copy_based(GraphNode *host_nodes, int N, int start_id)
{
    /* Reset visited */
    for (int i = 0; i < N; i++)
        host_nodes[i].visited = 0;

    /* Allocate device buffer for one level of nodes */
    size_t level_buf_size = (size_t)N * sizeof(GraphNode);
    CUdeviceptr dev_buf = 0;
    CUresult rc = cuMemAlloc_v2(&dev_buf, level_buf_size);
    if (rc != CUDA_SUCCESS) {
        fprintf(stderr, "    [WARN] cuMemAlloc for copy-BFS failed (%d)\n", rc);
        return -1;
    }

    /* Index-based neighbor offsets: store node index * sizeof(GraphNode) */
    /* Frontier as index list */
    int *frontier = malloc((size_t)N * sizeof(int));
    int *next_frontier = malloc((size_t)N * sizeof(int));
    if (!frontier || !next_frontier) {
        free(frontier);
        free(next_frontier);
        cuMemFree_v2(dev_buf);
        return -1;
    }

    int front_size = 0, next_size = 0;
    int visited_count = 0;

    /* Start */
    host_nodes[start_id].visited = 1;
    frontier[front_size++] = start_id;
    visited_count++;

    /* Staging buffer for batched copy */
    GraphNode *stage = malloc((size_t)N * sizeof(GraphNode));
    if (!stage) { free(frontier); free(next_frontier); cuMemFree_v2(dev_buf); return -1; }

    while (front_size > 0) {
        /* Batch: gather frontier nodes into contiguous staging buffer */
        for (int f = 0; f < front_size; f++)
            stage[f] = host_nodes[frontier[f]];

        /* Single bulk copy to device */
        cuMemcpyHtoD_v2(dev_buf, stage,
                         (size_t)front_size * sizeof(GraphNode));

        /* Single bulk copy back ("device processed") */
        cuMemcpyDtoH_v2(stage, dev_buf,
                         (size_t)front_size * sizeof(GraphNode));

        GraphNode *tmp = stage;

        next_size = 0;
        for (int f = 0; f < front_size; f++) {
            for (uint32_t j = 0; j < tmp[f].num_neighbors && j < MAX_NEIGHBORS; j++) {
                /* In copy-based mode, neighbor_offsets stores node indices */
                int nb_idx = (int)tmp[f].neighbor_offsets[j];
                if (nb_idx < 0 || nb_idx >= N) continue;
                if (!host_nodes[nb_idx].visited) {
                    host_nodes[nb_idx].visited = 1;
                    visited_count++;
                    if (next_size < N) {
                        next_frontier[next_size++] = nb_idx;
                    }
                }
            }
        }
        /* Swap frontiers */
        int *swap_tmp = frontier;
        frontier = next_frontier;
        next_frontier = swap_tmp;
        front_size = next_size;
    }

    free(stage);
    free(frontier);
    free(next_frontier);
    cuMemFree_v2(dev_buf);
    return visited_count;
}

/* ---- Run BFS experiment for a given graph type and device fraction */

static void run_bfs_experiment(const char *graph_type, int N, double dev_frac)
{
    printf("  graph=%s  N=%d  device_fraction=%.2f\n", graph_type, N, dev_frac);

    int dev_count = (int)(dev_frac * N);
    if (dev_count > N) dev_count = N;

    /* ---- Allocate nodes ------------------------------------------ */
    /* Coherent pool for device-fraction nodes */
    void *coh_pool = NULL;
    size_t coh_size = (size_t)dev_count * sizeof(GraphNode);
    if (dev_count > 0) {
        int ret = cxlCoherentAlloc((uint64_t)coh_size, &coh_pool);
        if (ret != CUDA_SUCCESS || !coh_pool) {
            /* Retry with smaller N */
            int new_dev = dev_count / 2;
            fprintf(stderr, "    [WARN] cxlCoherentAlloc(%zu) failed, "
                    "reducing device nodes %d -> %d\n",
                    coh_size, dev_count, new_dev);
            dev_count = new_dev;
            coh_size = (size_t)dev_count * sizeof(GraphNode);
            if (dev_count > 0) {
                ret = cxlCoherentAlloc((uint64_t)coh_size, &coh_pool);
                if (ret != CUDA_SUCCESS || !coh_pool) {
                    fprintf(stderr, "    [ERROR] cxlCoherentAlloc still failed, "
                            "skipping\n");
                    return;
                }
            }
        }
    }

    int host_count = N - dev_count;
    GraphNode *host_nodes = NULL;
    if (host_count > 0) {
        host_nodes = (GraphNode *)calloc((size_t)host_count, sizeof(GraphNode));
        if (!host_nodes) {
            fprintf(stderr, "    [ERROR] malloc for host nodes failed\n");
            if (coh_pool) cxlCoherentFree(coh_pool);
            return;
        }
    }

    /* Build offset table: for each node, store its device offset.
     * Device-resident nodes have real BAR4 offsets.
     * Host-resident nodes: we cast their host pointer to a uint64_t
     * "pseudo-offset" for the coherent BFS; for copy-based BFS we
     * use indices instead.
     */
    uint64_t *node_offsets = malloc((size_t)N * sizeof(uint64_t));
    if (!node_offsets) {
        fprintf(stderr, "    [ERROR] malloc for offset table failed\n");
        free(host_nodes);
        if (coh_pool) cxlCoherentFree(coh_pool);
        return;
    }

    /* Compute offsets for coherent nodes */
    GraphNode *coh_nodes = (GraphNode *)coh_pool;
    for (int i = 0; i < dev_count; i++) {
        node_offsets[i] = host_to_dev_offset(&coh_nodes[i]);
    }
    /* For host nodes in the coherent BFS we need them in coherent memory
     * too -- if dev_frac < 1.0 we allocate a second coherent region for
     * the "host" portion so that cxlDeviceToHost works uniformly.
     * (In a real CXL system host DRAM is also coherently visible; here
     *  we keep everything in BAR4 but set different bias modes.)
     */
    void *host_coh_pool = NULL;
    size_t host_coh_size = (size_t)host_count * sizeof(GraphNode);
    if (host_count > 0) {
        int ret = cxlCoherentAlloc((uint64_t)host_coh_size, &host_coh_pool);
        if (ret != CUDA_SUCCESS || !host_coh_pool) {
            fprintf(stderr, "    [WARN] cxlCoherentAlloc for host portion "
                    "failed (%zu bytes), falling back to copy-only\n",
                    host_coh_size);
            /* Mark that coherent BFS cannot run with mixed placement */
            host_coh_pool = NULL;
        }
    }

    GraphNode *host_coh_nodes = (GraphNode *)host_coh_pool;
    for (int i = 0; i < host_count; i++) {
        if (host_coh_pool) {
            node_offsets[dev_count + i] =
                host_to_dev_offset(&host_coh_nodes[i]);
        } else {
            /* Placeholder: coherent BFS will be skipped */
            node_offsets[dev_count + i] = 0;
        }
    }

    /* Set bias: device-fraction nodes get device bias,
     * host-fraction nodes get host bias */
    if (coh_pool && dev_count > 0) {
        cxlSetBias(coh_pool, (uint64_t)coh_size, CXL_BIAS_DEVICE);
    }
    if (host_coh_pool && host_count > 0) {
        cxlSetBias(host_coh_pool, (uint64_t)host_coh_size, CXL_BIAS_HOST);
    }

    /* ---- Build graphs -------------------------------------------- */
    /* Coherent graph (for Method A) */
    int can_run_coherent = (coh_pool != NULL || host_coh_pool != NULL);
    if (dev_frac > 0.0 && dev_frac < 1.0 && !host_coh_pool)
        can_run_coherent = 0;

    /* For coherent version, nodes are at coh_nodes[0..dev_count-1] and
     * host_coh_nodes[0..host_count-1]. We build using node_offsets. */
    GraphNode *all_coherent = NULL;
    if (can_run_coherent) {
        /* Temporary staging area: we need a flat array of N GraphNode
         * to pass to the graph builder, then copy into coherent memory. */
        all_coherent = (GraphNode *)calloc((size_t)N, sizeof(GraphNode));
        if (!all_coherent) {
            fprintf(stderr, "    [ERROR] calloc for staging graph failed\n");
            can_run_coherent = 0;
        }
    }

    if (can_run_coherent) {
        rng_seed(42);
        if (strcmp(graph_type, "erdos_renyi") == 0) {
            build_erdos_renyi(all_coherent, N, node_offsets);
        } else {
            build_barabasi_albert(all_coherent, N, node_offsets);
        }
        /* Copy built nodes into coherent regions */
        for (int i = 0; i < dev_count; i++) {
            memcpy(&coh_nodes[i], &all_coherent[i], sizeof(GraphNode));
        }
        for (int i = 0; i < host_count; i++) {
            if (host_coh_nodes) {
                memcpy(&host_coh_nodes[i], &all_coherent[dev_count + i],
                       sizeof(GraphNode));
            }
        }
        cxlCoherentFence();
    }

    /* Copy-based graph (for Method B): lives in host_nodes with
     * neighbor_offsets storing node indices */
    GraphNode *copy_graph = (GraphNode *)calloc((size_t)N, sizeof(GraphNode));
    int can_run_copy = (copy_graph != NULL);
    if (can_run_copy) {
        /* Build index-based offset table */
        uint64_t *idx_offsets = malloc((size_t)N * sizeof(uint64_t));
        if (!idx_offsets) {
            can_run_copy = 0;
        } else {
            for (int i = 0; i < N; i++)
                idx_offsets[i] = (uint64_t)i;

            rng_seed(42);  /* Same seed for comparable graphs */
            if (strcmp(graph_type, "erdos_renyi") == 0) {
                build_erdos_renyi(copy_graph, N, idx_offsets);
            } else {
                build_barabasi_albert(copy_graph, N, idx_offsets);
            }
            free(idx_offsets);
        }
    }

    /* ---- Run trials ---------------------------------------------- */
    uint64_t coh_times[NUM_TRIALS];
    uint64_t copy_times[NUM_TRIALS];
    struct timespec t0, t1;

    /* Method A: Pointer-sharing (coherent) */
    if (can_run_coherent) {
        cxlResetCoherencyStats();
        for (int trial = 0; trial < NUM_TRIALS; trial++) {
            clock_gettime(CLOCK_MONOTONIC, &t0);
            bfs_coherent(node_offsets, N, 0);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            coh_times[trial] = elapsed_ns(&t0, &t1);
        }

        uint64_t med, p25, p75;
        compute_stats(coh_times, NUM_TRIALS, &med, &p25, &p75);
        printf("    Method A (pointer-sharing):  median=%lu us  "
               "p25=%lu us  p75=%lu us\n",
               (unsigned long)(med / 1000),
               (unsigned long)(p25 / 1000),
               (unsigned long)(p75 / 1000));
        print_coherency_stats("    ");
    } else {
        printf("    Method A (pointer-sharing):  SKIPPED "
               "(coherent alloc unavailable)\n");
    }

    /* Method B: Copy-based */
    if (can_run_copy) {
        cxlResetCoherencyStats();
        for (int trial = 0; trial < NUM_TRIALS; trial++) {
            clock_gettime(CLOCK_MONOTONIC, &t0);
            bfs_copy_based(copy_graph, N, 0);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            copy_times[trial] = elapsed_ns(&t0, &t1);
        }

        uint64_t med, p25, p75;
        compute_stats(copy_times, NUM_TRIALS, &med, &p25, &p75);
        printf("    Method B (copy-based):       median=%lu us  "
               "p25=%lu us  p75=%lu us\n",
               (unsigned long)(med / 1000),
               (unsigned long)(p25 / 1000),
               (unsigned long)(p75 / 1000));
        print_coherency_stats("    ");
    } else {
        printf("    Method B (copy-based):       SKIPPED "
               "(allocation failed)\n");
    }

    /* ---- Cleanup ------------------------------------------------- */
    free(all_coherent);
    free(copy_graph);
    free(node_offsets);
    free(host_nodes);
    if (host_coh_pool) cxlCoherentFree(host_coh_pool);
    if (coh_pool) cxlCoherentFree(coh_pool);
}

static void experiment1_bfs(int N)
{
    printf("\n");
    printf("================================================================\n");
    printf("  EXPERIMENT 1: BFS -- Pointer Sharing vs Copy-Based Offload\n");
    printf("  N=%d  avg_degree=%d  trials=%d\n", N, AVG_DEGREE, NUM_TRIALS);
    printf("================================================================\n");

    static const double fracs[] = { 0.0, 0.25, 0.50, 0.75, 1.0 };
    static const char *graph_types[] = { "erdos_renyi", "barabasi_albert" };
    int n_fracs = (int)(sizeof(fracs) / sizeof(fracs[0]));
    int n_types = (int)(sizeof(graph_types) / sizeof(graph_types[0]));

    for (int g = 0; g < n_types; g++) {
        printf("\n--- Graph type: %s ---\n", graph_types[g]);
        for (int f = 0; f < n_fracs; f++) {
            run_bfs_experiment(graph_types[g], N, fracs[f]);
            printf("\n");
        }
    }
}

/* ================================================================== */
/*   EXPERIMENT 2:  Simple B+ tree benchmark                          */
/* ================================================================== */

/*
 * B+ tree:
 *   - Internal nodes in host memory
 *   - Leaf nodes in coherent device memory
 *   - Branching factor B (configurable)
 */

#define BTREE_MAX_B     256
#define BTREE_NUM_KEYS  10000
#define BTREE_NUM_OPS   5000

typedef struct BTreeLeaf {
    uint64_t keys[BTREE_MAX_B];
    uint64_t vals[BTREE_MAX_B];
    uint32_t count;
    uint32_t _pad;
    uint64_t next_offset;   /* BAR4 offset of next leaf, 0 = none */
} BTreeLeaf;

typedef struct BTreeInternal {
    uint64_t keys[BTREE_MAX_B];
    void    *children[BTREE_MAX_B + 1]; /* host pointers for internal,
                                           device offsets for leaves */
    uint32_t count;
    uint32_t is_leaf_level;             /* 1 if children are leaves */
} BTreeInternal;

/* Simplified B+ tree: single level of internal nodes pointing to leaves.
 * This is sufficient to measure the coherent vs copy-back difference
 * for leaf access patterns. */
typedef struct BTree {
    BTreeInternal *root;
    void          *leaf_pool;       /* coherent pool for leaves */
    BTreeLeaf     *leaves;          /* pointer into leaf_pool */
    int            num_leaves;
    int            B;               /* branching factor (keys per leaf) */
} BTree;

static BTree *btree_create(int B, int num_leaves)
{
    BTree *tree = calloc(1, sizeof(BTree));
    if (!tree) return NULL;

    tree->B = B;
    tree->num_leaves = num_leaves;

    /* Allocate leaves in coherent memory */
    size_t leaf_size = (size_t)num_leaves * sizeof(BTreeLeaf);
    int ret = cxlCoherentAlloc((uint64_t)leaf_size, &tree->leaf_pool);
    if (ret != CUDA_SUCCESS || !tree->leaf_pool) {
        fprintf(stderr, "    [WARN] B-tree leaf alloc failed (%zu bytes)\n",
                leaf_size);
        free(tree);
        return NULL;
    }
    tree->leaves = (BTreeLeaf *)tree->leaf_pool;
    memset(tree->leaves, 0, leaf_size);

    /* Set device bias for leaves */
    cxlSetBias(tree->leaf_pool, (uint64_t)leaf_size, CXL_BIAS_DEVICE);

    /* Single internal root node in host memory */
    tree->root = calloc(1, sizeof(BTreeInternal));
    if (!tree->root) {
        cxlCoherentFree(tree->leaf_pool);
        free(tree);
        return NULL;
    }
    tree->root->is_leaf_level = 1;
    tree->root->count = 0;

    return tree;
}

static void btree_destroy(BTree *tree)
{
    if (!tree) return;
    free(tree->root);
    if (tree->leaf_pool) cxlCoherentFree(tree->leaf_pool);
    free(tree);
}

/* Insert key into tree.  Simplified: distribute keys round-robin into
 * pre-allocated leaves sorted by key ranges. */
static void btree_bulk_insert(BTree *tree, uint64_t *keys, int nkeys)
{
    /* Sort keys */
    qsort(keys, (size_t)nkeys, sizeof(uint64_t), cmp_u64);

    int per_leaf = tree->B;
    int leaf_idx = 0;

    /* Fill leaves */
    for (int i = 0; i < nkeys && leaf_idx < tree->num_leaves; ) {
        BTreeLeaf *leaf = &tree->leaves[leaf_idx];
        leaf->count = 0;
        int fill = 0;
        while (fill < per_leaf && i < nkeys) {
            leaf->keys[fill] = keys[i];
            leaf->vals[fill] = keys[i] * 17 + 3;  /* dummy value */
            fill++;
            i++;
        }
        leaf->count = (uint32_t)fill;
        if (leaf_idx + 1 < tree->num_leaves) {
            leaf->next_offset =
                host_to_dev_offset(&tree->leaves[leaf_idx + 1]);
        } else {
            leaf->next_offset = 0;
        }
        leaf_idx++;
    }

    /* Build internal root: store first key of each leaf */
    tree->root->count = 0;
    for (int i = 0; i < leaf_idx && (int)tree->root->count < BTREE_MAX_B; i++) {
        if (i > 0) {
            tree->root->keys[tree->root->count] = tree->leaves[i].keys[0];
            tree->root->count++;
        }
        tree->root->children[i] = (void *)(uintptr_t)
            host_to_dev_offset(&tree->leaves[i]);
    }

    cxlCoherentFence();
}

/* Lookup via pointer sharing: traverse internal node (host), then
 * access leaf via cxlDeviceToHost */
static int btree_lookup_coherent(BTree *tree, uint64_t key, uint64_t *val)
{
    /* Find child in internal node */
    int child_idx = 0;
    for (uint32_t i = 0; i < tree->root->count; i++) {
        if (key >= tree->root->keys[i])
            child_idx = (int)i + 1;
        else
            break;
    }

    uint64_t leaf_off = (uint64_t)(uintptr_t)tree->root->children[child_idx];
    BTreeLeaf *leaf = (BTreeLeaf *)cxlDeviceToHost(leaf_off);
    if (!leaf) return -1;

    /* Linear scan within leaf */
    for (uint32_t i = 0; i < leaf->count; i++) {
        if (leaf->keys[i] == key) {
            *val = leaf->vals[i];
            return 0;
        }
    }
    return -1;  /* Not found */
}

/* Lookup via copy-back: copy leaf to host, then search */
static int btree_lookup_copy(BTree *tree, uint64_t key, uint64_t *val)
{
    int child_idx = 0;
    for (uint32_t i = 0; i < tree->root->count; i++) {
        if (key >= tree->root->keys[i])
            child_idx = (int)i + 1;
        else
            break;
    }

    uint64_t leaf_off = (uint64_t)(uintptr_t)tree->root->children[child_idx];

    /* Allocate device buffer and copy leaf */
    CUdeviceptr dev_ptr = 0;
    CUresult rc = cuMemAlloc_v2(&dev_ptr, sizeof(BTreeLeaf));
    if (rc != CUDA_SUCCESS) return -1;

    /* Copy from coherent memory to device, then to host buffer */
    BTreeLeaf local_leaf;
    /* In the copy-based model we read the leaf from device memory.
     * The leaf lives at leaf_off in the coherent pool (BAR4), so
     * we treat it as device memory and copy to host. */
    cuMemcpyDtoH_v2(&local_leaf, leaf_off, sizeof(BTreeLeaf));
    cuMemFree_v2(dev_ptr);

    for (uint32_t i = 0; i < local_leaf.count; i++) {
        if (local_leaf.keys[i] == key) {
            *val = local_leaf.vals[i];
            return 0;
        }
    }
    return -1;
}

static void experiment2_btree(void)
{
    printf("\n");
    printf("================================================================\n");
    printf("  EXPERIMENT 2: B+ Tree -- Pointer Sharing vs Copy-Back\n");
    printf("  keys=%d  lookup_ops=%d  trials=%d\n",
           BTREE_NUM_KEYS, BTREE_NUM_OPS, NUM_TRIALS);
    printf("================================================================\n");

    static const int branching[] = { 4, 16, 64, 256 };
    int n_branching = (int)(sizeof(branching) / sizeof(branching[0]));

    /* Generate random keys */
    uint64_t *keys = malloc((size_t)BTREE_NUM_KEYS * sizeof(uint64_t));
    if (!keys) {
        fprintf(stderr, "  [ERROR] malloc for keys failed\n");
        return;
    }
    rng_seed(0xBEEF);
    for (int i = 0; i < BTREE_NUM_KEYS; i++)
        keys[i] = xorshift64() % 1000000ULL;

    /* Generate lookup keys (subset of inserted keys) */
    uint64_t *lookup_keys = malloc((size_t)BTREE_NUM_OPS * sizeof(uint64_t));
    if (!lookup_keys) {
        free(keys);
        fprintf(stderr, "  [ERROR] malloc for lookup keys failed\n");
        return;
    }
    for (int i = 0; i < BTREE_NUM_OPS; i++)
        lookup_keys[i] = keys[xorshift64() % (uint64_t)BTREE_NUM_KEYS];

    for (int bi = 0; bi < n_branching; bi++) {
        int B = branching[bi];
        int num_leaves = (BTREE_NUM_KEYS + B - 1) / B;

        printf("\n  B=%d  num_leaves=%d\n", B, num_leaves);

        BTree *tree = btree_create(B, num_leaves);
        if (!tree) {
            printf("    SKIPPED (tree creation failed)\n");
            continue;
        }

        /* Make a mutable copy of keys for sorting */
        uint64_t *keys_copy = malloc((size_t)BTREE_NUM_KEYS * sizeof(uint64_t));
        if (!keys_copy) {
            btree_destroy(tree);
            printf("    SKIPPED (malloc failed)\n");
            continue;
        }
        memcpy(keys_copy, keys, (size_t)BTREE_NUM_KEYS * sizeof(uint64_t));
        btree_bulk_insert(tree, keys_copy, BTREE_NUM_KEYS);
        free(keys_copy);

        /* Pointer-sharing lookups */
        uint64_t coh_times[NUM_TRIALS];
        uint64_t copy_times[NUM_TRIALS];
        struct timespec t0, t1;

        cxlResetCoherencyStats();
        for (int trial = 0; trial < NUM_TRIALS; trial++) {
            uint64_t val;
            int found = 0;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int op = 0; op < BTREE_NUM_OPS; op++) {
                if (btree_lookup_coherent(tree, lookup_keys[op], &val) == 0)
                    found++;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            coh_times[trial] = elapsed_ns(&t0, &t1);
            (void)found;
        }

        uint64_t med, p25, p75;
        compute_stats(coh_times, NUM_TRIALS, &med, &p25, &p75);
        printf("    Pointer-sharing lookup:  median=%lu us  p25=%lu us  "
               "p75=%lu us  (%lu ns/op)\n",
               (unsigned long)(med / 1000),
               (unsigned long)(p25 / 1000),
               (unsigned long)(p75 / 1000),
               (unsigned long)(med / BTREE_NUM_OPS));
        print_coherency_stats("    ");

        /* Copy-based lookups */
        cxlResetCoherencyStats();
        for (int trial = 0; trial < NUM_TRIALS; trial++) {
            uint64_t val;
            int found = 0;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int op = 0; op < BTREE_NUM_OPS; op++) {
                if (btree_lookup_copy(tree, lookup_keys[op], &val) == 0)
                    found++;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            copy_times[trial] = elapsed_ns(&t0, &t1);
            (void)found;
        }

        compute_stats(copy_times, NUM_TRIALS, &med, &p25, &p75);
        printf("    Copy-based lookup:      median=%lu us  p25=%lu us  "
               "p75=%lu us  (%lu ns/op)\n",
               (unsigned long)(med / 1000),
               (unsigned long)(p25 / 1000),
               (unsigned long)(p75 / 1000),
               (unsigned long)(med / BTREE_NUM_OPS));
        print_coherency_stats("    ");

        btree_destroy(tree);
    }

    free(lookup_keys);
    free(keys);
}

/* ================================================================== */
/*   EXPERIMENT 3:  Chained hash table                                */
/* ================================================================== */

#define HASH_NUM_BUCKETS    1024
#define HASH_NUM_OPS        20000
#define HASH_CHAIN_MAX      16

typedef struct HashEntry {
    uint64_t key;
    uint64_t value;
    uint64_t next_offset;       /* BAR4 offset of next entry, 0 = end */
    uint64_t _pad;              /* pad to 32 bytes */
} HashEntry;

typedef struct HashBucket {
    uint64_t head_offset;       /* BAR4 offset of first HashEntry, 0 = empty */
    uint64_t _pad[3];           /* pad to 32 bytes */
} HashBucket;

typedef struct HashTable {
    void       *bucket_pool;    /* coherent allocation for buckets */
    HashBucket *buckets;
    void       *entry_pool;     /* coherent allocation for entries */
    HashEntry  *entries;
    int         num_buckets;
    int         entry_cap;
    int         entry_used;
} HashTable;

static uint64_t hash_fn(uint64_t key, int num_buckets)
{
    /* murmur-style finalizer */
    key ^= key >> 33;
    key *= 0xFF51AFD7ED558CCDULL;
    key ^= key >> 33;
    key *= 0xC4CEB9FE1A85EC53ULL;
    key ^= key >> 33;
    return key % (uint64_t)num_buckets;
}

static HashTable *hash_create(int num_buckets, int entry_cap)
{
    HashTable *ht = calloc(1, sizeof(HashTable));
    if (!ht) return NULL;

    ht->num_buckets = num_buckets;
    ht->entry_cap = entry_cap;
    ht->entry_used = 0;

    /* Buckets in coherent memory */
    size_t bucket_size = (size_t)num_buckets * sizeof(HashBucket);
    int ret = cxlCoherentAlloc((uint64_t)bucket_size, &ht->bucket_pool);
    if (ret != CUDA_SUCCESS || !ht->bucket_pool) {
        fprintf(stderr, "    [WARN] hash bucket alloc failed\n");
        free(ht);
        return NULL;
    }
    ht->buckets = (HashBucket *)ht->bucket_pool;
    memset(ht->buckets, 0, bucket_size);

    /* Entries in coherent memory */
    size_t entry_size = (size_t)entry_cap * sizeof(HashEntry);
    ret = cxlCoherentAlloc((uint64_t)entry_size, &ht->entry_pool);
    if (ret != CUDA_SUCCESS || !ht->entry_pool) {
        fprintf(stderr, "    [WARN] hash entry alloc failed\n");
        cxlCoherentFree(ht->bucket_pool);
        free(ht);
        return NULL;
    }
    ht->entries = (HashEntry *)ht->entry_pool;
    memset(ht->entries, 0, entry_size);

    /* Set bias: buckets device-biased, entries host-biased initially */
    cxlSetBias(ht->bucket_pool, (uint64_t)bucket_size, CXL_BIAS_DEVICE);
    cxlSetBias(ht->entry_pool, (uint64_t)entry_size, CXL_BIAS_HOST);
    cxlCoherentFence();

    return ht;
}

static void hash_destroy(HashTable *ht)
{
    if (!ht) return;
    if (ht->entry_pool) cxlCoherentFree(ht->entry_pool);
    if (ht->bucket_pool) cxlCoherentFree(ht->bucket_pool);
    free(ht);
}

/*
 * Coherent put: follow chain pointers via cxlDeviceToHost,
 * allocate new entry from the coherent pool.
 */
static int hash_put_coherent(HashTable *ht, uint64_t key, uint64_t value)
{
    uint64_t idx = hash_fn(key, ht->num_buckets);
    HashBucket *bkt = &ht->buckets[idx];

    /* Walk existing chain to check for update */
    uint64_t cur_off = bkt->head_offset;
    while (cur_off != 0) {
        HashEntry *entry = (HashEntry *)cxlDeviceToHost(cur_off);
        if (!entry) break;
        if (entry->key == key) {
            entry->value = value;
            return 0;
        }
        cur_off = entry->next_offset;
    }

    /* Allocate new entry */
    if (ht->entry_used >= ht->entry_cap) return -1;
    HashEntry *new_entry = &ht->entries[ht->entry_used++];
    new_entry->key = key;
    new_entry->value = value;
    new_entry->next_offset = bkt->head_offset;
    bkt->head_offset = host_to_dev_offset(new_entry);

    return 0;
}

/*
 * Coherent get: follow chain pointers via cxlDeviceToHost.
 */
static int hash_get_coherent(HashTable *ht, uint64_t key, uint64_t *value)
{
    uint64_t idx = hash_fn(key, ht->num_buckets);
    HashBucket *bkt = &ht->buckets[idx];

    uint64_t cur_off = bkt->head_offset;
    while (cur_off != 0) {
        HashEntry *entry = (HashEntry *)cxlDeviceToHost(cur_off);
        if (!entry) break;
        if (entry->key == key) {
            *value = entry->value;
            return 0;
        }
        cur_off = entry->next_offset;
    }
    return -1;
}

/*
 * Copy-based put: copy bucket from device, modify locally, copy back.
 */
static int hash_put_copy(HashTable *ht, uint64_t key, uint64_t value)
{
    uint64_t idx = hash_fn(key, ht->num_buckets);
    uint64_t bkt_off = host_to_dev_offset(&ht->buckets[idx]);

    /* Copy bucket from device */
    HashBucket local_bkt;
    cuMemcpyDtoH_v2(&local_bkt, bkt_off, sizeof(HashBucket));

    /* Walk chain via copies */
    uint64_t cur_off = local_bkt.head_offset;
    while (cur_off != 0) {
        HashEntry local_entry;
        cuMemcpyDtoH_v2(&local_entry, cur_off, sizeof(HashEntry));
        if (local_entry.key == key) {
            local_entry.value = value;
            cuMemcpyHtoD_v2(cur_off, &local_entry, sizeof(HashEntry));
            return 0;
        }
        cur_off = local_entry.next_offset;
    }

    /* Allocate new entry */
    if (ht->entry_used >= ht->entry_cap) return -1;
    HashEntry *new_entry = &ht->entries[ht->entry_used++];
    uint64_t new_off = host_to_dev_offset(new_entry);

    HashEntry local_new;
    local_new.key = key;
    local_new.value = value;
    local_new.next_offset = local_bkt.head_offset;
    local_new._pad = 0;
    cuMemcpyHtoD_v2(new_off, &local_new, sizeof(HashEntry));

    local_bkt.head_offset = new_off;
    cuMemcpyHtoD_v2(bkt_off, &local_bkt, sizeof(HashBucket));

    return 0;
}

/*
 * Copy-based get: copy bucket and chain entries from device.
 */
static int hash_get_copy(HashTable *ht, uint64_t key, uint64_t *value)
{
    uint64_t idx = hash_fn(key, ht->num_buckets);
    uint64_t bkt_off = host_to_dev_offset(&ht->buckets[idx]);

    HashBucket local_bkt;
    cuMemcpyDtoH_v2(&local_bkt, bkt_off, sizeof(HashBucket));

    uint64_t cur_off = local_bkt.head_offset;
    int chain_len = 0;
    while (cur_off != 0 && chain_len < HASH_CHAIN_MAX) {
        HashEntry local_entry;
        cuMemcpyDtoH_v2(&local_entry, cur_off, sizeof(HashEntry));
        if (local_entry.key == key) {
            *value = local_entry.value;
            return 0;
        }
        cur_off = local_entry.next_offset;
        chain_len++;
    }
    return -1;
}

static void experiment3_hashtable(void)
{
    printf("\n");
    printf("================================================================\n");
    printf("  EXPERIMENT 3: Chained Hash Table -- Coherent vs Copy-Based\n");
    printf("  buckets=%d  ops=%d  trials=%d\n",
           HASH_NUM_BUCKETS, HASH_NUM_OPS, NUM_TRIALS);
    printf("================================================================\n");

    /* Generate keys and values */
    rng_seed(0xCAFE);
    uint64_t *op_keys = malloc((size_t)HASH_NUM_OPS * sizeof(uint64_t));
    uint64_t *op_vals = malloc((size_t)HASH_NUM_OPS * sizeof(uint64_t));
    if (!op_keys || !op_vals) {
        free(op_keys);
        free(op_vals);
        fprintf(stderr, "  [ERROR] malloc for hash ops failed\n");
        return;
    }
    for (int i = 0; i < HASH_NUM_OPS; i++) {
        op_keys[i] = xorshift64() % 100000ULL;
        op_vals[i] = xorshift64();
    }

    /* --- Coherent hash table --- */
    printf("\n  [A] Pointer-sharing (coherent):\n");
    {
        uint64_t put_times[NUM_TRIALS];
        uint64_t get_times[NUM_TRIALS];
        struct timespec t0, t1;

        for (int trial = 0; trial < NUM_TRIALS; trial++) {
            HashTable *ht = hash_create(HASH_NUM_BUCKETS, HASH_NUM_OPS);
            if (!ht) {
                printf("    SKIPPED (hash table creation failed)\n");
                goto hash_copy;
            }

            /* Put phase */
            cxlResetCoherencyStats();
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int i = 0; i < HASH_NUM_OPS; i++) {
                hash_put_coherent(ht, op_keys[i], op_vals[i]);
            }
            cxlCoherentFence();
            clock_gettime(CLOCK_MONOTONIC, &t1);
            put_times[trial] = elapsed_ns(&t0, &t1);

            /* Get phase */
            clock_gettime(CLOCK_MONOTONIC, &t0);
            uint64_t val;
            int found = 0;
            for (int i = 0; i < HASH_NUM_OPS; i++) {
                if (hash_get_coherent(ht, op_keys[i], &val) == 0)
                    found++;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            get_times[trial] = elapsed_ns(&t0, &t1);
            (void)found;

            hash_destroy(ht);
        }

        uint64_t med, p25, p75;
        compute_stats(put_times, NUM_TRIALS, &med, &p25, &p75);
        printf("    PUT: median=%lu us  p25=%lu us  p75=%lu us  "
               "(%lu ns/op)\n",
               (unsigned long)(med / 1000),
               (unsigned long)(p25 / 1000),
               (unsigned long)(p75 / 1000),
               (unsigned long)(med / HASH_NUM_OPS));

        compute_stats(get_times, NUM_TRIALS, &med, &p25, &p75);
        printf("    GET: median=%lu us  p25=%lu us  p75=%lu us  "
               "(%lu ns/op)\n",
               (unsigned long)(med / 1000),
               (unsigned long)(p25 / 1000),
               (unsigned long)(p75 / 1000),
               (unsigned long)(med / HASH_NUM_OPS));
        print_coherency_stats("    ");
    }

hash_copy:
    /* --- Copy-based hash table --- */
    printf("\n  [B] Copy-based:\n");
    {
        uint64_t put_times[NUM_TRIALS];
        uint64_t get_times[NUM_TRIALS];
        struct timespec t0, t1;

        for (int trial = 0; trial < NUM_TRIALS; trial++) {
            HashTable *ht = hash_create(HASH_NUM_BUCKETS, HASH_NUM_OPS);
            if (!ht) {
                printf("    SKIPPED (hash table creation failed)\n");
                goto hash_done;
            }

            /* Put phase */
            cxlResetCoherencyStats();
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int i = 0; i < HASH_NUM_OPS; i++) {
                hash_put_copy(ht, op_keys[i], op_vals[i]);
            }
            cxlCoherentFence();
            clock_gettime(CLOCK_MONOTONIC, &t1);
            put_times[trial] = elapsed_ns(&t0, &t1);

            /* Get phase */
            clock_gettime(CLOCK_MONOTONIC, &t0);
            uint64_t val;
            int found = 0;
            for (int i = 0; i < HASH_NUM_OPS; i++) {
                if (hash_get_copy(ht, op_keys[i], &val) == 0)
                    found++;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            get_times[trial] = elapsed_ns(&t0, &t1);
            (void)found;

            hash_destroy(ht);
        }

        uint64_t med, p25, p75;
        compute_stats(put_times, NUM_TRIALS, &med, &p25, &p75);
        printf("    PUT: median=%lu us  p25=%lu us  p75=%lu us  "
               "(%lu ns/op)\n",
               (unsigned long)(med / 1000),
               (unsigned long)(p25 / 1000),
               (unsigned long)(p75 / 1000),
               (unsigned long)(med / HASH_NUM_OPS));

        compute_stats(get_times, NUM_TRIALS, &med, &p25, &p75);
        printf("    GET: median=%lu us  p25=%lu us  p75=%lu us  "
               "(%lu ns/op)\n",
               (unsigned long)(med / 1000),
               (unsigned long)(p25 / 1000),
               (unsigned long)(p75 / 1000),
               (unsigned long)(med / HASH_NUM_OPS));
        print_coherency_stats("    ");
    }

hash_done:
    free(op_keys);
    free(op_vals);
}

/* ================================================================== */
/*   Main                                                             */
/* ================================================================== */

int main(int argc, char **argv)
{
    int N = DEFAULT_NUM_NODES;

    if (argc > 1) {
        int n = atoi(argv[1]);
        if (n > 0) N = n;
    }

    printf("================================================================\n");
    printf("  RQ1: Pointer Sharing vs Copy-Based Offload\n");
    printf("  Pointer-Rich Workloads on CXL Type-2 Coherent Memory\n");
    printf("================================================================\n");

    /* ---- Initialize CUDA / CXL context ---- */
    CUresult err = cuInit(0);
    if (err != CUDA_SUCCESS) {
        fprintf(stderr, "FATAL: cuInit failed (%d)\n", err);
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

    /* Compute BAR4 base for offset calculations */
    init_bar4_base();
    if (!g_bar4_base) {
        fprintf(stderr, "FATAL: could not determine BAR4 base address\n");
        return 1;
    }

    printf("  BAR4 base: %p\n", (void *)g_bar4_base);
    printf("  sizeof(GraphNode) = %zu (expect 64)\n", sizeof(GraphNode));
    if (sizeof(GraphNode) != 64) {
        fprintf(stderr, "WARNING: GraphNode is %zu bytes, expected 64\n",
                sizeof(GraphNode));
    }

    /* ---- Run experiments ---- */
    /* Env RQ1_EXPS (comma list of 1/2/3, default "1,2,3") selects which to run. */
    int want_exp1 = 1, want_exp2 = 1, want_exp3 = 1;
    const char *exps_env = getenv("RQ1_EXPS");
    if (exps_env && *exps_env) {
        want_exp1 = want_exp2 = want_exp3 = 0;
        for (const char *p = exps_env; *p; p++) {
            if (*p == '1') want_exp1 = 1;
            else if (*p == '2') want_exp2 = 1;
            else if (*p == '3') want_exp3 = 1;
        }
    }
    if (want_exp1) experiment1_bfs(N);
    if (want_exp2) experiment2_btree();
    if (want_exp3) experiment3_hashtable();

    printf("\n================================================================\n");
    printf("  All experiments complete.\n");
    printf("================================================================\n");

    return 0;
}
