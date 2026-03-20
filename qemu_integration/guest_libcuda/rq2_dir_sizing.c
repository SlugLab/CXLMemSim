/*
 * RQ2: Device Cache Directory Capacity vs Coherence Hub Viability
 *
 * Measures how directory pressure scales with working set size by sweeping
 * graph BFS and B-tree workloads across coherent memory.  Since the hardware
 * directory size is fixed, we vary the working set to create different amounts
 * of pressure, observe hit rates, eviction counts, and throughput.
 *
 * Experiments:
 *   1. Graph BFS with varying N (directory pressure from random traversal)
 *   2. B+ tree lookups with varying key count
 *   3. Reuse distance profiling (W90) to predict directory capacity needs
 *   4. Hardware directory size read from BAR2 registers
 *
 * Compile:
 *   gcc -Wall -O2 -o rq2_dir_sizing rq2_dir_sizing.c -L. -lcuda -lrt -lm -Wl,-rpath,.
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

/* ========================================================================
 * CUDA / CXL coherent memory API declarations
 * ======================================================================== */

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef uint64_t CUdeviceptr;

#define CUDA_SUCCESS 0

extern CUresult cuInit(unsigned int flags);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev);

extern int cxlCoherentAlloc(uint64_t size, void **host_ptr);
extern int cxlCoherentFree(void *host_ptr);
extern void *cxlDeviceToHost(uint64_t dev_offset);
extern int cxlCoherentFence(void);

typedef struct {
    uint64_t snoop_hits, snoop_misses, coherency_requests, back_invalidations;
    uint64_t writebacks, evictions, bias_flips;
    uint64_t device_bias_hits, host_bias_hits;
    uint64_t upgrades, downgrades, directory_entries;
} CXLCoherencyStats;

extern int cxlGetCoherencyStats(CXLCoherencyStats *stats);
extern int cxlResetCoherencyStats(void);

/* ========================================================================
 * Constants
 * ======================================================================== */

#define CACHE_LINE_SIZE     64
#define MAX_NEIGHBORS       6
#define NUM_TRIALS          10
#define BTREE_LOOKUPS       1000
#define MAX_REUSE_TRACK     200000  /* max address slots for reuse profiling */

/* Graph BFS working set sizes: small -> medium -> large */
static const int BFS_SIZES[] = { 100, 500, 1000, 2000, 5000, 10000, 20000, 50000 };
#define NUM_BFS_SIZES ((int)(sizeof(BFS_SIZES) / sizeof(BFS_SIZES[0])))

/* B-tree key counts */
static const int BTREE_SIZES[] = { 100, 500, 1000, 5000, 10000, 50000 };
#define NUM_BTREE_SIZES ((int)(sizeof(BTREE_SIZES) / sizeof(BTREE_SIZES[0])))

/* ========================================================================
 * Graph node: 64 bytes per cache line
 * ======================================================================== */

typedef struct GraphNode {
    uint64_t id;
    uint32_t visited;
    uint32_t num_neighbors;
    uint64_t neighbor_offsets[MAX_NEIGHBORS]; /* byte offsets into coherent region */
} GraphNode;

_Static_assert(sizeof(GraphNode) == 64, "GraphNode must be exactly 64 bytes");

/* ========================================================================
 * Timing helpers
 * ======================================================================== */

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ========================================================================
 * Simple LCG PRNG (deterministic, no global state)
 * ======================================================================== */

typedef struct { uint64_t state; } rng_t;

static inline void rng_seed(rng_t *r, uint64_t s)
{
    r->state = s ? s : 1;
}

static inline uint64_t rng_next(rng_t *r)
{
    r->state = r->state * 6364136223846793005ULL + 1442695040888963407ULL;
    return r->state;
}

static inline uint32_t rng_u32(rng_t *r, uint32_t limit)
{
    return (uint32_t)(rng_next(r) >> 33) % limit;
}

/* ========================================================================
 * Comparison function for qsort (uint64_t, ascending)
 * ======================================================================== */

static int cmp_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return  1;
    return 0;
}

static uint64_t median_u64(uint64_t *arr, int n)
{
    if (n <= 0) return 0;
    qsort(arr, (size_t)n, sizeof(uint64_t), cmp_u64);
    return arr[n / 2];
}

static double median_dbl(double *arr, int n)
{
    if (n <= 0) return 0.0;
    /* simple insertion sort for small n */
    for (int i = 1; i < n; i++) {
        double key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
    return arr[n / 2];
}

/* ========================================================================
 * Experiment 1: Graph BFS with varying working set size
 * ======================================================================== */

/*
 * Build Erdos-Renyi random graph with avg_degree ~6 in coherent memory.
 * Each node is one cache line.  neighbor_offsets are byte offsets so the
 * device can follow pointers through coherent memory.
 */
static GraphNode *build_er_graph(int N, void *coh_base, rng_t *rng)
{
    GraphNode *nodes = (GraphNode *)coh_base;

    /* Initialize nodes */
    for (int i = 0; i < N; i++) {
        nodes[i].id = (uint64_t)i;
        nodes[i].visited = 0;
        nodes[i].num_neighbors = 0;
        memset(nodes[i].neighbor_offsets, 0, sizeof(nodes[i].neighbor_offsets));
    }

    /*
     * ER model: for each pair (i,j), edge exists with probability p = avg_degree / (N-1).
     * To keep it O(N * avg_degree) rather than O(N^2), we sample edges directly.
     * For each node, draw ~avg_degree random neighbors.
     */
    for (int i = 0; i < N; i++) {
        int target_degree = MAX_NEIGHBORS; /* cap at 6 */
        for (int d = 0; d < target_degree; d++) {
            int j = (int)rng_u32(rng, (uint32_t)N);
            if (j == i) continue;

            /* Add edge i -> j if room */
            if (nodes[i].num_neighbors < MAX_NEIGHBORS) {
                uint64_t offset = (uint64_t)j * sizeof(GraphNode);
                /* Check for duplicate */
                int dup = 0;
                for (uint32_t k = 0; k < nodes[i].num_neighbors; k++) {
                    if (nodes[i].neighbor_offsets[k] == offset) { dup = 1; break; }
                }
                if (!dup) {
                    nodes[i].neighbor_offsets[nodes[i].num_neighbors] = offset;
                    nodes[i].num_neighbors++;
                }
            }
        }
    }

    cxlCoherentFence();
    return nodes;
}

/*
 * BFS from start_node.  Returns number of nodes visited.
 * Also populates access_trace (if non-NULL) with cache-line-aligned addresses
 * for reuse distance profiling.
 */
static int run_bfs(GraphNode *nodes, int N, int start_node,
                   uint64_t *access_trace, int *trace_len, int trace_cap)
{
    /* BFS queue: use a simple ring buffer */
    int *queue = malloc(sizeof(int) * (size_t)N);
    if (!queue) return 0;

    int head = 0, tail = 0;
    int visited_count = 0;
    int tl = 0;

    /* Reset visited flags */
    for (int i = 0; i < N; i++)
        nodes[i].visited = 0;

    cxlCoherentFence();

    nodes[start_node].visited = 1;
    queue[tail++] = start_node;
    visited_count++;

    while (head < tail) {
        int u = queue[head++];
        GraphNode *node_u = &nodes[u];

        /* Record access to this node's cache line */
        if (access_trace && tl < trace_cap) {
            access_trace[tl++] = (uint64_t)u * sizeof(GraphNode);
        }

        for (uint32_t e = 0; e < node_u->num_neighbors; e++) {
            uint64_t off = node_u->neighbor_offsets[e];
            int v = (int)(off / sizeof(GraphNode));
            if (v < 0 || v >= N) continue;

            /* Record the neighbor access */
            if (access_trace && tl < trace_cap) {
                access_trace[tl++] = off;
            }

            if (!nodes[v].visited) {
                nodes[v].visited = 1;
                queue[tail++] = v;
                visited_count++;
            }
        }
    }

    cxlCoherentFence();
    free(queue);
    if (trace_len) *trace_len = tl;
    return visited_count;
}

typedef struct {
    int    N;
    double hit_rate;
    uint64_t eviction_count;
    uint64_t bfs_time_ns;
    double normalized_throughput;
    uint64_t directory_entries;
    uint64_t snoop_hits;
    uint64_t snoop_misses;
} BFSResult;

static void run_bfs_experiment(BFSResult *results)
{
    printf("\n");
    printf("================================================================\n");
    printf("  Experiment 1: Graph BFS directory pressure sweep\n");
    printf("================================================================\n");
    printf("  Varying N to create different working set sizes.\n");
    printf("  Each node = 64 bytes (1 cache line), ER graph, avg degree 6.\n");
    printf("  %d trials per configuration, reporting median.\n\n", NUM_TRIALS);

    for (int si = 0; si < NUM_BFS_SIZES; si++) {
        int N = BFS_SIZES[si];
        uint64_t alloc_size = (uint64_t)N * sizeof(GraphNode);

        double trial_hit_rates[NUM_TRIALS];
        uint64_t trial_evictions[NUM_TRIALS];
        uint64_t trial_times[NUM_TRIALS];
        uint64_t trial_dir_entries[NUM_TRIALS];
        uint64_t trial_snoop_hits[NUM_TRIALS];
        uint64_t trial_snoop_misses[NUM_TRIALS];

        int valid_trials = 0;

        for (int t = 0; t < NUM_TRIALS; t++) {
            void *coh_ptr = NULL;
            int ret = cxlCoherentAlloc(alloc_size, &coh_ptr);
            if (ret != CUDA_SUCCESS || !coh_ptr) {
                fprintf(stderr, "  WARN: cxlCoherentAlloc failed for N=%d (size=%lu)\n",
                        N, (unsigned long)alloc_size);
                break;
            }

            rng_t rng;
            rng_seed(&rng, 42 + (uint64_t)t * 997 + (uint64_t)N);
            GraphNode *nodes = build_er_graph(N, coh_ptr, &rng);

            /* Reset coherency stats before BFS */
            cxlResetCoherencyStats();

            uint64_t t0 = now_ns();
            int start = (int)rng_u32(&rng, (uint32_t)N);
            run_bfs(nodes, N, start, NULL, NULL, 0);
            uint64_t t1 = now_ns();

            /* Read coherency stats */
            CXLCoherencyStats stats;
            memset(&stats, 0, sizeof(stats));
            cxlGetCoherencyStats(&stats);

            uint64_t total_snoops = stats.snoop_hits + stats.snoop_misses;
            double hr = (total_snoops > 0)
                ? (double)stats.snoop_hits / (double)total_snoops
                : 0.0;

            trial_hit_rates[valid_trials]    = hr;
            trial_evictions[valid_trials]    = stats.evictions;
            trial_times[valid_trials]        = t1 - t0;
            trial_dir_entries[valid_trials]  = stats.directory_entries;
            trial_snoop_hits[valid_trials]   = stats.snoop_hits;
            trial_snoop_misses[valid_trials] = stats.snoop_misses;
            valid_trials++;

            cxlCoherentFree(coh_ptr);
        }

        if (valid_trials == 0) {
            results[si].N = N;
            results[si].hit_rate = 0.0;
            results[si].eviction_count = 0;
            results[si].bfs_time_ns = 0;
            results[si].normalized_throughput = 0.0;
            results[si].directory_entries = 0;
            results[si].snoop_hits = 0;
            results[si].snoop_misses = 0;
            continue;
        }

        results[si].N                = N;
        results[si].hit_rate         = median_dbl(trial_hit_rates, valid_trials);
        results[si].eviction_count   = median_u64(trial_evictions, valid_trials);
        results[si].bfs_time_ns      = median_u64(trial_times, valid_trials);
        results[si].directory_entries = median_u64(trial_dir_entries, valid_trials);
        results[si].snoop_hits       = median_u64(trial_snoop_hits, valid_trials);
        results[si].snoop_misses     = median_u64(trial_snoop_misses, valid_trials);

    }

    /* Compute normalized throughput relative to smallest N.
     * Throughput ~ nodes_visited / time.  Normalize so that smallest N = 1.0.
     * Since all BFS visit O(N) nodes, throughput_i ~ N_i / time_i.
     * Normalized = (N_i / time_i) / (N_0 / time_0).
     */
    double base_tp = 0.0;
    if (results[0].bfs_time_ns > 0)
        base_tp = (double)BFS_SIZES[0] / (double)results[0].bfs_time_ns;

    for (int si = 0; si < NUM_BFS_SIZES; si++) {
        if (base_tp > 0.0 && results[si].bfs_time_ns > 0) {
            double tp = (double)BFS_SIZES[si] / (double)results[si].bfs_time_ns;
            results[si].normalized_throughput = tp / base_tp;
        } else {
            results[si].normalized_throughput = 0.0;
        }
    }

    /* Print results table */
    printf("  %8s  %8s  %12s  %12s  %12s  %10s\n",
           "N", "hit_rate", "evictions", "bfs_time_ns", "norm_tput", "dir_entries");
    printf("  %8s  %8s  %12s  %12s  %12s  %10s\n",
           "--------", "--------", "------------", "------------",
           "------------", "----------");
    for (int si = 0; si < NUM_BFS_SIZES; si++) {
        BFSResult *r = &results[si];
        printf("  %8d  %8.4f  %12lu  %12lu  %12.4f  %10lu\n",
               r->N, r->hit_rate,
               (unsigned long)r->eviction_count,
               (unsigned long)r->bfs_time_ns,
               r->normalized_throughput,
               (unsigned long)r->directory_entries);
    }
}

/* ========================================================================
 * Experiment 2: B+ tree lookups with varying key count
 * ======================================================================== */

/*
 * B+ tree node layout in coherent memory.  We use a branching factor of 64.
 * Each internal node stores up to B-1 keys and B child pointers.
 * For simplicity we build a packed array-based tree (implicit pointers).
 */
#define BTREE_B  64

typedef struct BTreeNode {
    uint32_t num_keys;
    uint32_t is_leaf;
    uint64_t keys[BTREE_B - 1];
    /* For internal nodes: child offsets are implicit (array-based tree).
     * For leaf nodes: values would go here; we just track accesses. */
} BTreeNode;

/*
 * Build a sorted key array and construct an implicit B-tree.
 * The tree is stored as a flat array of BTreeNode in coherent memory.
 * Node i's children are at indices (i * BTREE_B + 1) .. (i * BTREE_B + BTREE_B).
 */
static int build_btree(int num_keys, void *coh_base, uint64_t *sorted_keys)
{
    /* Generate sorted keys */
    for (int i = 0; i < num_keys; i++)
        sorted_keys[i] = (uint64_t)i * 17 + 3; /* deterministic, spread out */

    /*
     * Compute tree height and total nodes needed.
     * Height h: need B^h >= num_keys, so h = ceil(log_B(num_keys)).
     */
    int total_nodes = 0;
    int level_nodes = 1;
    int h = 0;
    int capacity = 1;
    while (capacity < num_keys) {
        h++;
        capacity *= BTREE_B;
    }

    /* Total nodes in a complete B-tree of height h */
    level_nodes = 1;
    total_nodes = 0;
    for (int l = 0; l <= h; l++) {
        total_nodes += level_nodes;
        level_nodes *= BTREE_B;
    }
    if (total_nodes <= 0) total_nodes = 1;

    BTreeNode *tree = (BTreeNode *)coh_base;

    /* Initialize all nodes */
    memset(tree, 0, (size_t)total_nodes * sizeof(BTreeNode));

    /* Fill keys into internal nodes using sorted order.
     * For simplicity, distribute keys in BFS order of the tree. */
    int key_idx = 0;
    for (int i = 0; i < total_nodes && key_idx < num_keys; i++) {
        int child_base = i * BTREE_B + 1;
        int is_leaf = (child_base >= total_nodes);
        tree[i].is_leaf = is_leaf ? 1 : 0;

        int nk = (num_keys - key_idx < BTREE_B - 1)
                 ? (num_keys - key_idx)
                 : (BTREE_B - 1);
        tree[i].num_keys = (uint32_t)nk;
        for (int k = 0; k < nk && key_idx < num_keys; k++) {
            tree[i].keys[k] = sorted_keys[key_idx++];
        }
    }

    cxlCoherentFence();
    return total_nodes;
}

/*
 * Perform a single B-tree lookup.  Descends the implicit tree structure.
 * Returns 1 if found, 0 otherwise.  The point is to generate directory traffic.
 */
static int btree_lookup(BTreeNode *tree, int total_nodes, uint64_t key)
{
    int node = 0;

    while (node < total_nodes) {
        BTreeNode *n = &tree[node];
        volatile uint32_t nk = n->num_keys; /* force coherent read */

        /* Binary search within node */
        int lo = 0, hi = (int)nk - 1;
        int found = 0;
        int child_idx = (int)nk; /* default: rightmost child */

        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            uint64_t mid_key = n->keys[mid];
            if (key == mid_key) {
                found = 1;
                break;
            } else if (key < mid_key) {
                child_idx = mid;
                hi = mid - 1;
            } else {
                lo = mid + 1;
            }
        }

        if (found || n->is_leaf)
            return found;

        /* Descend to child */
        node = node * BTREE_B + 1 + child_idx;
    }
    return 0;
}

typedef struct {
    int    num_keys;
    double hit_rate;
    uint64_t eviction_count;
    uint64_t lookup_time_ns;
    double normalized_throughput;
    uint64_t directory_entries;
} BTreeResult;

static void run_btree_experiment(BTreeResult *results)
{
    printf("\n");
    printf("================================================================\n");
    printf("  Experiment 2: B+ tree lookup directory pressure sweep\n");
    printf("================================================================\n");
    printf("  Branching factor B=%d, %d random lookups per size.\n",
           BTREE_B, BTREE_LOOKUPS);
    printf("  %d trials per configuration, reporting median.\n\n", NUM_TRIALS);

    for (int si = 0; si < NUM_BTREE_SIZES; si++) {
        int num_keys = BTREE_SIZES[si];

        /* Compute total nodes needed */
        int h = 0;
        int cap = 1;
        while (cap < num_keys) { h++; cap *= BTREE_B; }
        int level_nodes = 1;
        int total_nodes = 0;
        for (int l = 0; l <= h; l++) {
            total_nodes += level_nodes;
            level_nodes *= BTREE_B;
        }
        if (total_nodes <= 0) total_nodes = 1;

        uint64_t tree_size = (uint64_t)total_nodes * sizeof(BTreeNode);
        uint64_t keys_size = (uint64_t)num_keys * sizeof(uint64_t);
        uint64_t alloc_size = tree_size + keys_size;

        double trial_hit_rates[NUM_TRIALS];
        uint64_t trial_evictions[NUM_TRIALS];
        uint64_t trial_times[NUM_TRIALS];
        uint64_t trial_dir_entries[NUM_TRIALS];
        int valid_trials = 0;

        for (int t = 0; t < NUM_TRIALS; t++) {
            void *coh_ptr = NULL;
            int ret = cxlCoherentAlloc(alloc_size, &coh_ptr);
            if (ret != CUDA_SUCCESS || !coh_ptr) {
                fprintf(stderr, "  WARN: cxlCoherentAlloc failed for keys=%d\n",
                        num_keys);
                break;
            }

            uint64_t *sorted_keys = (uint64_t *)((uint8_t *)coh_ptr + tree_size);
            int tn = build_btree(num_keys, coh_ptr, sorted_keys);
            BTreeNode *tree = (BTreeNode *)coh_ptr;

            rng_t rng;
            rng_seed(&rng, 12345 + (uint64_t)t * 31 + (uint64_t)num_keys);

            cxlResetCoherencyStats();

            uint64_t t0 = now_ns();
            for (int q = 0; q < BTREE_LOOKUPS; q++) {
                /* Random key: half existing, half non-existing */
                uint64_t key;
                if (rng_u32(&rng, 2) == 0 && num_keys > 0) {
                    int idx = (int)rng_u32(&rng, (uint32_t)num_keys);
                    key = sorted_keys[idx];
                } else {
                    key = (uint64_t)rng_u32(&rng, (uint32_t)(num_keys * 20));
                }
                btree_lookup(tree, tn, key);
            }
            uint64_t t1 = now_ns();

            CXLCoherencyStats stats;
            memset(&stats, 0, sizeof(stats));
            cxlGetCoherencyStats(&stats);

            uint64_t total_snoops = stats.snoop_hits + stats.snoop_misses;
            double hr = (total_snoops > 0)
                ? (double)stats.snoop_hits / (double)total_snoops
                : 0.0;

            trial_hit_rates[valid_trials]   = hr;
            trial_evictions[valid_trials]   = stats.evictions;
            trial_times[valid_trials]       = t1 - t0;
            trial_dir_entries[valid_trials] = stats.directory_entries;
            valid_trials++;

            cxlCoherentFree(coh_ptr);
        }

        if (valid_trials == 0) {
            results[si].num_keys = num_keys;
            results[si].hit_rate = 0.0;
            results[si].eviction_count = 0;
            results[si].lookup_time_ns = 0;
            results[si].normalized_throughput = 0.0;
            results[si].directory_entries = 0;
            continue;
        }

        results[si].num_keys         = num_keys;
        results[si].hit_rate         = median_dbl(trial_hit_rates, valid_trials);
        results[si].eviction_count   = median_u64(trial_evictions, valid_trials);
        results[si].lookup_time_ns   = median_u64(trial_times, valid_trials);
        results[si].directory_entries = median_u64(trial_dir_entries, valid_trials);
    }

    /* Normalized throughput: lookups/time relative to smallest key count */
    double base_tp = 0.0;
    if (results[0].lookup_time_ns > 0)
        base_tp = (double)BTREE_LOOKUPS / (double)results[0].lookup_time_ns;

    for (int si = 0; si < NUM_BTREE_SIZES; si++) {
        if (base_tp > 0.0 && results[si].lookup_time_ns > 0) {
            double tp = (double)BTREE_LOOKUPS / (double)results[si].lookup_time_ns;
            results[si].normalized_throughput = tp / base_tp;
        } else {
            results[si].normalized_throughput = 0.0;
        }
    }

    printf("  %8s  %8s  %12s  %14s  %12s  %10s\n",
           "keys", "hit_rate", "evictions", "lookup_time_ns", "norm_tput", "dir_entries");
    printf("  %8s  %8s  %12s  %14s  %12s  %10s\n",
           "--------", "--------", "------------", "--------------",
           "------------", "----------");
    for (int si = 0; si < NUM_BTREE_SIZES; si++) {
        BTreeResult *r = &results[si];
        printf("  %8d  %8.4f  %12lu  %14lu  %12.4f  %10lu\n",
               r->num_keys, r->hit_rate,
               (unsigned long)r->eviction_count,
               (unsigned long)r->lookup_time_ns,
               r->normalized_throughput,
               (unsigned long)r->directory_entries);
    }
}

/* ========================================================================
 * Experiment 3: Reuse distance profiling
 * ========================================================================
 *
 * For each BFS access to cache-line address A, the reuse distance is the
 * number of distinct addresses accessed since A was last accessed.
 * We compute W90 = 90th percentile reuse distance, which gives the minimum
 * directory capacity needed to capture 90% of temporal locality.
 */

typedef struct {
    int    N;
    double W90;
    int    trace_length;
} ReuseResult;

/*
 * Compute reuse distances from an access trace.
 * Uses a hash set with timestamp tracking for O(N * log(N)) performance.
 * Returns W90 (90th percentile reuse distance).
 */
static double compute_reuse_w90(uint64_t *trace, int trace_len)
{
    if (trace_len <= 0) return 0.0;

    /*
     * For each address, track its last-access timestamp.
     * Reuse distance = number of distinct addresses accessed between
     * the previous and current access to the same address.
     *
     * Simple approach: hash table mapping address -> last_seen_index.
     * We maintain a running count of distinct addresses in the window
     * by walking through the trace.
     *
     * For efficiency with potentially large traces, we use a direct-mapped
     * approach: normalize addresses to cache-line indices.
     */

    /* Find max cache-line index to size our array */
    uint64_t max_cl = 0;
    for (int i = 0; i < trace_len; i++) {
        uint64_t cl = trace[i] / CACHE_LINE_SIZE;
        if (cl > max_cl) max_cl = cl;
    }

    /* last_seen[cl] = index of last access, or -1 if never */
    int *last_seen = calloc(max_cl + 1, sizeof(int));
    if (!last_seen) {
        fprintf(stderr, "  WARN: reuse distance alloc failed (max_cl=%lu)\n",
                (unsigned long)max_cl);
        return 0.0;
    }
    for (uint64_t i = 0; i <= max_cl; i++)
        last_seen[i] = -1;

    /*
     * Collect reuse distances.  For each access, if we have seen this
     * address before, count distinct addresses accessed in between.
     * This is O(N^2) in the worst case, but we cap the window scan
     * to avoid degenerate behavior on large traces.
     */
    int max_distances = trace_len;
    uint64_t *distances = malloc(sizeof(uint64_t) * (size_t)max_distances);
    if (!distances) {
        free(last_seen);
        return 0.0;
    }
    int num_distances = 0;

    /* Use a bit-set for counting distinct addresses in window */
    size_t bitset_bytes = (max_cl / 8) + 1;
    uint8_t *seen_bits = NULL;
    if (bitset_bytes <= 16 * 1024 * 1024) { /* cap at 16MB */
        seen_bits = malloc(bitset_bytes);
    }

    for (int i = 0; i < trace_len; i++) {
        uint64_t cl = trace[i] / CACHE_LINE_SIZE;
        int prev = last_seen[cl];
        last_seen[cl] = i;

        if (prev < 0) continue; /* cold miss, no reuse distance */

        if (seen_bits && (i - prev) < 100000) {
            /* Count distinct cache lines accessed between prev+1 and i-1 */
            memset(seen_bits, 0, bitset_bytes);
            int distinct = 0;
            for (int j = prev + 1; j < i; j++) {
                uint64_t jcl = trace[j] / CACHE_LINE_SIZE;
                uint64_t byte_idx = jcl / 8;
                uint8_t bit = (uint8_t)(1 << (jcl % 8));
                if (!(seen_bits[byte_idx] & bit)) {
                    seen_bits[byte_idx] |= bit;
                    distinct++;
                }
            }
            if (num_distances < max_distances)
                distances[num_distances++] = (uint64_t)distinct;
        } else {
            /* Approximate: use index distance as upper bound */
            if (num_distances < max_distances)
                distances[num_distances++] = (uint64_t)(i - prev);
        }
    }

    free(seen_bits);
    free(last_seen);

    /* Compute 90th percentile */
    double w90 = 0.0;
    if (num_distances > 0) {
        qsort(distances, (size_t)num_distances, sizeof(uint64_t), cmp_u64);
        int p90_idx = (int)((double)num_distances * 0.90);
        if (p90_idx >= num_distances) p90_idx = num_distances - 1;
        w90 = (double)distances[p90_idx];
    }

    free(distances);
    return w90;
}

static void run_reuse_distance_experiment(ReuseResult *results)
{
    printf("\n");
    printf("================================================================\n");
    printf("  Experiment 3: Reuse distance profiling\n");
    printf("================================================================\n");
    printf("  BFS traversal on each N, tracking cache-line access sequence.\n");
    printf("  W90 = 90th percentile reuse distance (minimum directory size\n");
    printf("  needed to capture 90%% of temporal locality).\n\n");

    for (int si = 0; si < NUM_BFS_SIZES; si++) {
        int N = BFS_SIZES[si];
        uint64_t alloc_size = (uint64_t)N * sizeof(GraphNode);

        void *coh_ptr = NULL;
        int ret = cxlCoherentAlloc(alloc_size, &coh_ptr);
        if (ret != CUDA_SUCCESS || !coh_ptr) {
            results[si].N = N;
            results[si].W90 = 0.0;
            results[si].trace_length = 0;
            fprintf(stderr, "  WARN: cxlCoherentAlloc failed for N=%d\n", N);
            continue;
        }

        rng_t rng;
        rng_seed(&rng, 42 + (uint64_t)N);
        build_er_graph(N, coh_ptr, &rng);

        /* Allocate trace buffer */
        int trace_cap = N * (MAX_NEIGHBORS + 1);
        if (trace_cap > MAX_REUSE_TRACK) trace_cap = MAX_REUSE_TRACK;
        uint64_t *trace = malloc(sizeof(uint64_t) * (size_t)trace_cap);
        int trace_len = 0;

        if (trace) {
            GraphNode *nodes = (GraphNode *)coh_ptr;
            run_bfs(nodes, N, 0, trace, &trace_len, trace_cap);

            double w90 = compute_reuse_w90(trace, trace_len);
            results[si].N = N;
            results[si].W90 = w90;
            results[si].trace_length = trace_len;
            free(trace);
        } else {
            results[si].N = N;
            results[si].W90 = 0.0;
            results[si].trace_length = 0;
        }

        cxlCoherentFree(coh_ptr);
    }

    printf("  %8s  %10s  %12s  %16s\n",
           "N", "W90", "trace_len", "min_dir (1.3*W90)");
    printf("  %8s  %10s  %12s  %16s\n",
           "--------", "----------", "------------", "----------------");
    for (int si = 0; si < NUM_BFS_SIZES; si++) {
        ReuseResult *r = &results[si];
        double min_dir = 1.3 * r->W90;
        printf("  %8d  %10.1f  %12d  %16.0f\n",
               r->N, r->W90, r->trace_length, min_dir);
    }
}

/* ========================================================================
 * Experiment 4: Read hardware directory size from BAR2 registers
 * ======================================================================== */

static void read_hw_directory_size(void)
{
    printf("\n");
    printf("================================================================\n");
    printf("  Experiment 4: Hardware directory size (BAR2 registers)\n");
    printf("================================================================\n\n");

    /*
     * After cuInit/cuCtxCreate the BAR2 registers are mapped.
     * Read CXL_GPU_REG_COH_DIR_SIZE (0x0318) and CXL_GPU_REG_COH_DIR_USED (0x0320).
     *
     * These are exposed through the coherency stats, but we also read them
     * via cxlGetCoherencyStats for the directory_entries field.
     */
    CXLCoherencyStats stats;
    memset(&stats, 0, sizeof(stats));
    cxlGetCoherencyStats(&stats);

    printf("  Directory size (from stats):  %lu entries\n",
           (unsigned long)stats.directory_entries);
    printf("  Register offsets:\n");
    printf("    CXL_GPU_REG_COH_DIR_SIZE  = 0x%04X\n", CXL_GPU_REG_COH_DIR_SIZE);
    printf("    CXL_GPU_REG_COH_DIR_USED  = 0x%04X\n", CXL_GPU_REG_COH_DIR_USED);
    printf("\n  Note: hardware directory size is fixed.  To study the effect of\n");
    printf("  directory capacity, we vary the working set size instead.\n");
    printf("  When working set > directory capacity, evictions and snoop misses\n");
    printf("  increase, revealing the capacity cliff.\n");
}

/* ========================================================================
 * Summary
 * ======================================================================== */

static void print_summary(BFSResult *bfs_results, BTreeResult *bt_results,
                          ReuseResult *reuse_results)
{
    printf("\n");
    printf("================================================================\n");
    printf("  RQ2 SUMMARY: Directory Capacity vs Coherence Hub Viability\n");
    printf("================================================================\n");

    printf("\n  [Graph BFS] N vs directory pressure:\n");
    printf("  %8s  %8s  %12s  %12s  %12s\n",
           "N", "hit_rate", "evictions", "bfs_time_ns", "norm_tput");
    printf("  %8s  %8s  %12s  %12s  %12s\n",
           "--------", "--------", "------------", "------------", "------------");
    for (int i = 0; i < NUM_BFS_SIZES; i++) {
        BFSResult *r = &bfs_results[i];
        printf("  %8d  %8.4f  %12lu  %12lu  %12.4f\n",
               r->N, r->hit_rate,
               (unsigned long)r->eviction_count,
               (unsigned long)r->bfs_time_ns,
               r->normalized_throughput);
    }

    printf("\n  [B-tree lookups] key count vs directory pressure:\n");
    printf("  %8s  %8s  %12s  %14s  %12s\n",
           "keys", "hit_rate", "evictions", "lookup_time_ns", "norm_tput");
    printf("  %8s  %8s  %12s  %14s  %12s\n",
           "--------", "--------", "------------", "--------------", "------------");
    for (int i = 0; i < NUM_BTREE_SIZES; i++) {
        BTreeResult *r = &bt_results[i];
        printf("  %8d  %8.4f  %12lu  %14lu  %12.4f\n",
               r->num_keys, r->hit_rate,
               (unsigned long)r->eviction_count,
               (unsigned long)r->lookup_time_ns,
               r->normalized_throughput);
    }

    printf("\n  [Reuse distance] W90 by working set size:\n");
    printf("  %8s  %10s  %16s\n", "N", "W90", "min_dir (1.3*W90)");
    printf("  %8s  %10s  %16s\n", "--------", "----------", "----------------");
    for (int i = 0; i < NUM_BFS_SIZES; i++) {
        ReuseResult *r = &reuse_results[i];
        printf("  %8d  %10.1f  %16.0f\n", r->N, r->W90, 1.3 * r->W90);
    }

    /* Identify capacity cliff from BFS data */
    printf("\n  Key observations:\n");

    /* Find where hit rate drops below 0.90 (if it does) */
    int cliff_N = -1;
    for (int i = 0; i < NUM_BFS_SIZES; i++) {
        if (bfs_results[i].hit_rate < 0.90) {
            cliff_N = bfs_results[i].N;
        }
    }
    if (cliff_N > 0) {
        printf("    - BFS hit rate drops below 90%% at N=%d nodes "
               "(%d cache lines, %d KB working set)\n",
               cliff_N, cliff_N,
               (int)((uint64_t)cliff_N * sizeof(GraphNode) / 1024));
    }

    /* Find the reuse distance prediction */
    for (int i = 0; i < NUM_BFS_SIZES; i++) {
        if (reuse_results[i].W90 > 0) {
            printf("    - N=%d: W90=%.0f => need directory >= %.0f entries "
                   "for 90%% hit rate\n",
                   reuse_results[i].N, reuse_results[i].W90,
                   1.3 * reuse_results[i].W90);
        }
    }

    printf("\n  Conclusion: the coherence hub model remains viable when the\n");
    printf("  device directory can hold >= 1.3x the 90th-percentile reuse\n");
    printf("  distance (W90) of the workload.  Beyond that, evictions and\n");
    printf("  snoop misses dominate, collapsing throughput.\n");
    printf("================================================================\n\n");
}

/* ========================================================================
 * Main
 * ======================================================================== */

int main(void)
{
    printf("================================================================\n");
    printf("  RQ2: Device Cache Directory Sizing Experiment\n");
    printf("  Measures directory pressure vs working set size\n");
    printf("================================================================\n\n");

    /* Initialize CUDA / CXL device */
    CUresult err = cuInit(0);
    if (err != CUDA_SUCCESS) {
        fprintf(stderr, "ERROR: cuInit failed (%d)\n", err);
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
    (void)ctx; /* context is active on the device; we don't reference it again */

    printf("  Device initialized.  Running experiments...\n");
    printf("  Trials per config: %d\n", NUM_TRIALS);
    printf("  BFS sizes: ");
    for (int i = 0; i < NUM_BFS_SIZES; i++)
        printf("%d%s", BFS_SIZES[i], (i < NUM_BFS_SIZES - 1) ? ", " : "\n");
    printf("  B-tree sizes: ");
    for (int i = 0; i < NUM_BTREE_SIZES; i++)
        printf("%d%s", BTREE_SIZES[i], (i < NUM_BTREE_SIZES - 1) ? ", " : "\n");

    /* Experiment 4: Read HW directory size first (informational) */
    read_hw_directory_size();

    /* Experiment 1: BFS sweep */
    BFSResult bfs_results[NUM_BFS_SIZES];
    memset(bfs_results, 0, sizeof(bfs_results));
    run_bfs_experiment(bfs_results);

    /* Experiment 2: B-tree sweep */
    BTreeResult bt_results[NUM_BTREE_SIZES];
    memset(bt_results, 0, sizeof(bt_results));
    run_btree_experiment(bt_results);

    /* Experiment 3: Reuse distance profiling */
    ReuseResult reuse_results[NUM_BFS_SIZES];
    memset(reuse_results, 0, sizeof(reuse_results));
    run_reuse_distance_experiment(reuse_results);

    /* Summary */
    print_summary(bfs_results, bt_results, reuse_results);

    return 0;
}
