/*
 * RQ4: Allocation Policy vs. Coherence Hub Traffic
 *
 * Tests how node placement across CXL device memory (BAR4) and host DRAM
 * affects coherence traffic for pointer-rich graph and hash table workloads.
 *
 * Pool A = cxlCoherentAlloc (BAR4, device domain, access goes through CXL)
 * Pool B = malloc            (host DRAM, local access)
 *
 * Cross-domain pointer follows incur CXL coherence overhead.
 *
 * 4 allocation policies:
 *   1. Random: 50/50 per node
 *   2. Static Affinity (BFS proximity): first N/2 BFS-discovered -> pool A
 *   3. Topology-Aware (BFS bisection): co-locate BFS-close nodes
 *   4. Online Migration: start random, periodically migrate hot boundary nodes
 *
 * Compile: gcc -Wall -O2 -o rq4_alloc_policy rq4_alloc_policy.c \
 *          -L. -lcuda -lrt -lm -Wl,-rpath,.
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
/* CUDA / CXL types and external APIs                                 */
/* ------------------------------------------------------------------ */

typedef int      CUresult;
typedef int      CUdevice;
typedef void    *CUcontext;
typedef uint64_t CUdeviceptr;

#define CUDA_SUCCESS    0
#define CXL_BIAS_HOST   0
#define CXL_BIAS_DEVICE 1

extern CUresult cuInit(unsigned int flags);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev);

extern int   cxlCoherentAlloc(uint64_t size, void **host_ptr);
extern int   cxlCoherentFree(void *host_ptr);
extern void *cxlDeviceToHost(uint64_t dev_offset);
extern int   cxlCoherentFence(void);
extern int   cxlSetBias(void *host_ptr, uint64_t size, int bias_mode);

typedef struct {
    uint64_t snoop_hits, snoop_misses, coherency_requests, back_invalidations;
    uint64_t writebacks, evictions, bias_flips;
    uint64_t device_bias_hits, host_bias_hits;
    uint64_t upgrades, downgrades, directory_entries;
} CXLCoherencyStats;

extern int cxlGetCoherencyStats(CXLCoherencyStats *stats);
extern int cxlResetCoherencyStats(void);

/* ------------------------------------------------------------------ */
/* Experiment parameters (compile-time defaults, env-overridable)      */
/* ------------------------------------------------------------------ */

#define DEFAULT_NUM_NODES       5000
#define AVG_DEGREE              6
#define MAX_NEIGHBORS           6
#define NUM_BFS_ROOTS           10
#define NUM_TRIALS              10
#define MIGRATE_INTERVAL        1000    /* BFS ops between migration rounds  */
#define MIGRATE_TOP_K           50      /* nodes migrated per round          */

#define HT_NUM_BUCKETS          1000
#define HT_NUM_OPS              50000
#define HT_CHAIN_LEN_MAX        20

#define POOL_DEVICE             0       /* cxlCoherentAlloc (BAR4)           */
#define POOL_HOST               1       /* malloc (host DRAM)                */

/* ------------------------------------------------------------------ */
/* Graph node -- exactly 64 bytes (one cache line)                     */
/* ------------------------------------------------------------------ */

typedef struct GraphNode {
    uint64_t id;                        /*  8 bytes */
    uint32_t visited;                   /*  4 bytes */
    uint32_t num_neighbors;             /*  4 bytes */
    uint64_t neighbor_offsets[MAX_NEIGHBORS]; /* 48 bytes */
} GraphNode;                            /* total: 64 bytes */

/* ------------------------------------------------------------------ */
/* Per-node metadata (kept in host DRAM, not part of cache-line node)  */
/* ------------------------------------------------------------------ */

typedef struct {
    int      pool;              /* POOL_DEVICE or POOL_HOST              */
    void    *ptr;               /* actual pointer (BAR4 or malloc)       */
    uint64_t handle;            /* opaque: device offset or host addr    */
    uint64_t access_count;      /* total accesses across all BFS runs    */
    uint64_t cross_domain_cnt;  /* cross-domain pointer follows          */
} NodeMeta;

/* ------------------------------------------------------------------ */
/* Global experiment state                                             */
/* ------------------------------------------------------------------ */

static int        g_num_nodes;
static NodeMeta  *g_meta;          /* g_num_nodes entries                  */

/* Device pool: one large cxlCoherentAlloc, sub-allocate from it.       */
static void      *g_dev_pool;      /* base pointer (BAR4-mapped)          */
static uint64_t   g_dev_pool_size;
static uint64_t   g_dev_pool_used;

/* Comparison function for qsort (descending by uint64_t value) */
static int cmp_access_desc(const void *a, const void *b)
{
    const uint64_t va = *(const uint64_t *)a;
    const uint64_t vb = *(const uint64_t *)b;
    if (vb > va) return  1;
    if (vb < va) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Time helpers                                                        */
/* ------------------------------------------------------------------ */

static uint64_t time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static double ns_to_ms(uint64_t ns) { return (double)ns / 1e6; }

/* ------------------------------------------------------------------ */
/* Simple PRNG (xorshift64)                                            */
/* ------------------------------------------------------------------ */

static uint64_t g_rng_state = 0x123456789ABCDEFULL;

static void rng_seed(uint64_t s) { g_rng_state = s ? s : 1; }

static uint64_t rng_next(void)
{
    uint64_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng_state = x;
    return x;
}

static int rng_int(int n) { return (int)(rng_next() % (uint64_t)n); }

/* ------------------------------------------------------------------ */
/* Device pool sub-allocator                                           */
/* ------------------------------------------------------------------ */

static int dev_pool_init(void)
{
    g_dev_pool_size = (uint64_t)g_num_nodes * sizeof(GraphNode);
    g_dev_pool_used = 0;
    int ret = cxlCoherentAlloc(g_dev_pool_size, &g_dev_pool);
    if (ret != CUDA_SUCCESS || !g_dev_pool) {
        fprintf(stderr, "ERROR: cxlCoherentAlloc(%lu) failed: %d\n",
                (unsigned long)g_dev_pool_size, ret);
        return -1;
    }
    /* Set device-bias for the whole pool */
    cxlSetBias(g_dev_pool, g_dev_pool_size, CXL_BIAS_DEVICE);
    return 0;
}

static void *dev_pool_alloc(uint64_t size, uint64_t *out_handle)
{
    uint64_t aligned = (size + 63) & ~63ULL; /* cache-line align */
    if (g_dev_pool_used + aligned > g_dev_pool_size)
        return NULL;
    void *ptr = (uint8_t *)g_dev_pool + g_dev_pool_used;
    *out_handle = g_dev_pool_used; /* offset within BAR4 mapping */
    g_dev_pool_used += aligned;
    return ptr;
}

static void dev_pool_reset(void)
{
    g_dev_pool_used = 0;
}

static void dev_pool_destroy(void)
{
    if (g_dev_pool) {
        cxlCoherentFree(g_dev_pool);
        g_dev_pool = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Resolve a neighbor_offset handle -> usable pointer                  */
/*                                                                     */
/* Encoding: bit 63 = 0 -> device pool offset (resolve via BAR4 base) */
/*           bit 63 = 1 -> host pointer cast to uint64_t               */
/*           0 = NULL (no neighbor)                                    */
/* ------------------------------------------------------------------ */

#define HANDLE_NULL      0ULL
#define HANDLE_HOST_BIT  (1ULL << 63)

static inline uint64_t make_device_handle(uint64_t dev_offset)
{
    /* dev_offset is relative to g_dev_pool; we store it directly.
       Bit 63 is 0 for any reasonable offset (<= 62-bit address space). */
    return dev_offset + 1; /* +1 so that offset 0 is distinguishable from NULL */
}

static inline uint64_t make_host_handle(void *ptr)
{
    return HANDLE_HOST_BIT | (uint64_t)(uintptr_t)ptr;
}

static inline GraphNode *resolve_handle(uint64_t h)
{
    if (h == HANDLE_NULL)
        return NULL;
    if (h & HANDLE_HOST_BIT)
        return (GraphNode *)(uintptr_t)(h & ~HANDLE_HOST_BIT);
    /* Device handle: offset into g_dev_pool */
    uint64_t off = h - 1;
    return (GraphNode *)((uint8_t *)g_dev_pool + off);
}

/* ------------------------------------------------------------------ */
/* Graph construction (adjacency stored in the node itself)            */
/* ------------------------------------------------------------------ */

/* Allocate a single node in the chosen pool */
static int alloc_node(int idx, int pool)
{
    NodeMeta *m = &g_meta[idx];
    m->pool = pool;
    m->access_count = 0;
    m->cross_domain_cnt = 0;

    if (pool == POOL_DEVICE) {
        uint64_t handle;
        void *p = dev_pool_alloc(sizeof(GraphNode), &handle);
        if (!p) return -1;
        m->ptr = p;
        m->handle = make_device_handle(handle);
    } else {
        void *p = malloc(sizeof(GraphNode));
        if (!p) return -1;
        m->ptr = p;
        m->handle = make_host_handle(p);
    }

    GraphNode *node = (GraphNode *)m->ptr;
    memset(node, 0, sizeof(*node));
    node->id = (uint64_t)idx;
    return 0;
}

static void free_host_nodes(void)
{
    for (int i = 0; i < g_num_nodes; i++) {
        if (g_meta[i].pool == POOL_HOST && g_meta[i].ptr) {
            free(g_meta[i].ptr);
            g_meta[i].ptr = NULL;
        }
    }
}

/* Wire edges: Erdos-Renyi-style with target average degree.
   Each node gets roughly AVG_DEGREE neighbors (capped at MAX_NEIGHBORS). */
static void wire_edges(void)
{
    /* For each node, pick AVG_DEGREE random neighbors */
    for (int i = 0; i < g_num_nodes; i++) {
        GraphNode *node = (GraphNode *)g_meta[i].ptr;
        node->num_neighbors = 0;

        for (int d = 0; d < AVG_DEGREE; d++) {
            int j = rng_int(g_num_nodes);
            if (j == i) { j = (i + 1) % g_num_nodes; }

            /* Avoid duplicate neighbors */
            int dup = 0;
            for (uint32_t k = 0; k < node->num_neighbors; k++) {
                if (node->neighbor_offsets[k] == g_meta[j].handle) {
                    dup = 1;
                    break;
                }
            }
            if (dup) continue;

            if (node->num_neighbors < MAX_NEIGHBORS) {
                node->neighbor_offsets[node->num_neighbors++] = g_meta[j].handle;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* BFS traversal -- returns nodes visited, tracks cross-domain stats   */
/* ------------------------------------------------------------------ */

static int *g_bfs_queue;          /* pre-allocated queue (g_num_nodes)   */

static uint64_t bfs_run(int root, uint64_t *out_cross_domain)
{
    /* Reset visited flags */
    for (int i = 0; i < g_num_nodes; i++) {
        GraphNode *n = (GraphNode *)g_meta[i].ptr;
        n->visited = 0;
    }

    int head = 0, tail = 0;
    uint64_t cross = 0;
    uint64_t visited = 0;

    GraphNode *root_node = (GraphNode *)g_meta[root].ptr;
    root_node->visited = 1;
    g_bfs_queue[tail++] = root;

    while (head < tail) {
        int cur_idx = g_bfs_queue[head++];
        GraphNode *cur = (GraphNode *)g_meta[cur_idx].ptr;
        int cur_pool = g_meta[cur_idx].pool;
        visited++;
        g_meta[cur_idx].access_count++;

        for (uint32_t e = 0; e < cur->num_neighbors; e++) {
            uint64_t nh = cur->neighbor_offsets[e];
            if (nh == HANDLE_NULL) continue;

            GraphNode *nbr = resolve_handle(nh);
            if (!nbr) continue;

            /* Determine neighbor's pool by looking up meta via id */
            int nbr_idx = (int)nbr->id;
            if (nbr_idx < 0 || nbr_idx >= g_num_nodes) continue;

            /* Count cross-domain access */
            if (g_meta[nbr_idx].pool != cur_pool) {
                cross++;
                g_meta[nbr_idx].cross_domain_cnt++;
            }

            if (!nbr->visited) {
                nbr->visited = 1;
                if (tail < g_num_nodes)
                    g_bfs_queue[tail++] = nbr_idx;
            }
        }
    }

    *out_cross_domain = cross;
    return visited;
}

/* ------------------------------------------------------------------ */
/* BFS ordering helper (used by affinity/topology policies)            */
/* ------------------------------------------------------------------ */

static int *bfs_order(int root, int *out_count)
{
    int *order = (int *)malloc(g_num_nodes * sizeof(int));
    uint8_t *seen = (uint8_t *)calloc(g_num_nodes, 1);
    if (!order || !seen) {
        free(order);
        free(seen);
        *out_count = 0;
        return NULL;
    }

    int head = 0, tail = 0;
    order[tail++] = root;
    seen[root] = 1;

    /* We need a simple adjacency for the BFS ordering.
       For policies 2 and 3 we haven't wired edges yet, so we use a
       preliminary random graph adjacency.  Build a temporary edge list. */

    /* Actually, for the ordering step we generate a quick random
       neighbor list on-the-fly using the same RNG seed pattern so that
       the ordering is deterministic for the same seed. */

    /* Simpler: just use a pre-computed adjacency.  We'll call this
       BEFORE wiring, so we build a temporary adjacency here. */

    /* Temporary neighbor lists: each node gets AVG_DEGREE random neighbors */
    uint64_t saved_rng = g_rng_state;
    rng_seed(42); /* deterministic seed for ordering */

    int **tmp_adj = (int **)malloc(g_num_nodes * sizeof(int *));
    int *tmp_deg  = (int *)calloc(g_num_nodes, sizeof(int));
    if (!tmp_adj || !tmp_deg) {
        free(order); free(seen); free(tmp_adj); free(tmp_deg);
        g_rng_state = saved_rng;
        *out_count = 0;
        return NULL;
    }
    for (int i = 0; i < g_num_nodes; i++) {
        tmp_adj[i] = (int *)malloc(AVG_DEGREE * sizeof(int));
        if (!tmp_adj[i]) {
            for (int k = 0; k < i; k++) free(tmp_adj[k]);
            free(tmp_adj); free(tmp_deg); free(order); free(seen);
            g_rng_state = saved_rng;
            *out_count = 0;
            return NULL;
        }
        for (int d = 0; d < AVG_DEGREE; d++) {
            int j = rng_int(g_num_nodes);
            if (j == i) j = (i + 1) % g_num_nodes;
            tmp_adj[i][tmp_deg[i]++] = j;
        }
    }

    while (head < tail) {
        int v = order[head++];
        for (int d = 0; d < tmp_deg[v]; d++) {
            int w = tmp_adj[v][d];
            if (!seen[w]) {
                seen[w] = 1;
                order[tail++] = w;
            }
        }
    }

    /* Free temp adjacency */
    for (int i = 0; i < g_num_nodes; i++) free(tmp_adj[i]);
    free(tmp_adj);
    free(tmp_deg);
    free(seen);

    g_rng_state = saved_rng;
    *out_count = tail;
    return order;
}

/* ------------------------------------------------------------------ */
/* Allocation policies                                                 */
/* ------------------------------------------------------------------ */

static int policy_random(void)
{
    for (int i = 0; i < g_num_nodes; i++) {
        int pool = (rng_int(2) == 0) ? POOL_DEVICE : POOL_HOST;
        if (alloc_node(i, pool) != 0) return -1;
    }
    return 0;
}

static int policy_static_affinity(void)
{
    /* BFS from node 0; first N/2 discovered -> device, rest -> host */
    int count = 0;
    int *order = bfs_order(0, &count);
    if (!order) return -1;

    int half = g_num_nodes / 2;
    for (int i = 0; i < count; i++) {
        int pool = (i < half) ? POOL_DEVICE : POOL_HOST;
        if (alloc_node(order[i], pool) != 0) { free(order); return -1; }
    }
    /* Any nodes not reached by BFS go to host */
    for (int i = 0; i < g_num_nodes; i++) {
        if (!g_meta[i].ptr) {
            if (alloc_node(i, POOL_HOST) != 0) { free(order); return -1; }
        }
    }
    free(order);
    return 0;
}

static int policy_topology_aware(void)
{
    /* Same BFS bisection as static affinity, but the BFS traversal root
       for the actual experiment will also start in pool A, so early
       nodes are local.  The placement is identical to static affinity;
       the difference is that experiment BFS roots are chosen from pool A. */
    return policy_static_affinity(); /* placement identical */
}

static int policy_online_migration_init(void)
{
    /* Start with random placement */
    return policy_random();
}

/* Migrate node idx from its current pool to the other pool.
   Returns 0 on success. */
static int migrate_node(int idx)
{
    NodeMeta *m = &g_meta[idx];
    GraphNode *old_node = (GraphNode *)m->ptr;
    GraphNode tmp;
    memcpy(&tmp, old_node, sizeof(GraphNode));

    int new_pool = (m->pool == POOL_DEVICE) ? POOL_HOST : POOL_DEVICE;

    /* Allocate in new pool */
    void *new_ptr = NULL;
    uint64_t new_handle = HANDLE_NULL;

    if (new_pool == POOL_DEVICE) {
        uint64_t off;
        new_ptr = dev_pool_alloc(sizeof(GraphNode), &off);
        if (!new_ptr) return -1;
        new_handle = make_device_handle(off);
    } else {
        new_ptr = malloc(sizeof(GraphNode));
        if (!new_ptr) return -1;
        new_handle = make_host_handle(new_ptr);
    }

    /* Copy data */
    memcpy(new_ptr, &tmp, sizeof(GraphNode));

    /* Free old if host (device pool is bump-allocated, no individual free) */
    if (m->pool == POOL_HOST) {
        free(m->ptr);
    }

    uint64_t old_handle = m->handle;

    m->pool = new_pool;
    m->ptr = new_ptr;
    m->handle = new_handle;
    m->cross_domain_cnt = 0; /* reset after migration */

    /* Update all pointers that reference this node */
    for (int i = 0; i < g_num_nodes; i++) {
        GraphNode *n = (GraphNode *)g_meta[i].ptr;
        for (uint32_t e = 0; e < n->num_neighbors; e++) {
            if (n->neighbor_offsets[e] == old_handle) {
                n->neighbor_offsets[e] = new_handle;
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Result structure for one trial                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    double   total_time_ms;
    uint64_t total_visited;
    uint64_t cross_domain_accesses;
    uint64_t coherency_requests;
    uint64_t evictions;
    double   throughput;  /* nodes/sec */
} TrialResult;

/* ------------------------------------------------------------------ */
/* Run BFS experiment for a given policy                               */
/* ------------------------------------------------------------------ */

static int run_bfs_experiment(const char *policy_name, int policy_id,
                              TrialResult *results, int num_trials)
{
    printf("\n  --- Policy: %s ---\n", policy_name);

    for (int t = 0; t < num_trials; t++) {
        /* Reset device pool for each trial */
        dev_pool_reset();
        free_host_nodes();
        memset(g_meta, 0, g_num_nodes * sizeof(NodeMeta));

        rng_seed((uint64_t)(t + 1) * 0xDEADBEEF + (uint64_t)policy_id * 0x1234);

        /* Allocate nodes according to policy */
        int rc = 0;
        switch (policy_id) {
        case 0: rc = policy_random();               break;
        case 1: rc = policy_static_affinity();       break;
        case 2: rc = policy_topology_aware();        break;
        case 3: rc = policy_online_migration_init(); break;
        default: return -1;
        }
        if (rc != 0) {
            fprintf(stderr, "  Trial %d: allocation failed\n", t);
            return -1;
        }

        /* Wire edges */
        wire_edges();
        cxlCoherentFence();

        /* Pick BFS roots */
        int roots[NUM_BFS_ROOTS];
        if (policy_id == 2) {
            /* Topology-aware: pick roots from pool A */
            int ri = 0;
            for (int i = 0; i < g_num_nodes && ri < NUM_BFS_ROOTS; i++) {
                if (g_meta[i].pool == POOL_DEVICE) {
                    roots[ri++] = i;
                }
            }
            /* Fill remaining with arbitrary nodes if needed */
            for (int i = 0; ri < NUM_BFS_ROOTS; i++) {
                roots[ri++] = i % g_num_nodes;
            }
        } else {
            /* Spread roots across the node space */
            for (int r = 0; r < NUM_BFS_ROOTS; r++) {
                roots[r] = (r * (g_num_nodes / NUM_BFS_ROOTS)) % g_num_nodes;
            }
        }

        /* Reset stats */
        cxlResetCoherencyStats();
        for (int i = 0; i < g_num_nodes; i++) {
            g_meta[i].access_count = 0;
            g_meta[i].cross_domain_cnt = 0;
        }

        uint64_t total_visited = 0;
        uint64_t total_cross = 0;
        uint64_t t_start = time_ns();

        for (int r = 0; r < NUM_BFS_ROOTS; r++) {
            uint64_t cross = 0;
            uint64_t vis = bfs_run(roots[r], &cross);
            total_visited += vis;
            total_cross += cross;

            /* Online migration: after every MIGRATE_INTERVAL cumulative ops */
            if (policy_id == 3 && (r + 1) % (MIGRATE_INTERVAL / NUM_BFS_ROOTS + 1) == 0) {
                /* Sort meta by cross-domain count, migrate top-K */
                /* We need index-preserving sort, so build an index array */
                int *sorted_idx = (int *)malloc(g_num_nodes * sizeof(int));
                if (sorted_idx) {
                    for (int i = 0; i < g_num_nodes; i++) sorted_idx[i] = i;
                    /* Simple selection of top-K (avoid full qsort) */
                    for (int k = 0; k < MIGRATE_TOP_K && k < g_num_nodes; k++) {
                        int best = k;
                        for (int j = k + 1; j < g_num_nodes; j++) {
                            if (g_meta[sorted_idx[j]].cross_domain_cnt >
                                g_meta[sorted_idx[best]].cross_domain_cnt) {
                                best = j;
                            }
                        }
                        if (best != k) {
                            int tmp = sorted_idx[k];
                            sorted_idx[k] = sorted_idx[best];
                            sorted_idx[best] = tmp;
                        }
                        if (g_meta[sorted_idx[k]].cross_domain_cnt > 0) {
                            migrate_node(sorted_idx[k]);
                        }
                    }
                    free(sorted_idx);
                }
            }
        }

        uint64_t t_end = time_ns();
        uint64_t elapsed_ns = t_end - t_start;

        CXLCoherencyStats stats;
        cxlGetCoherencyStats(&stats);

        results[t].total_time_ms = ns_to_ms(elapsed_ns);
        results[t].total_visited = total_visited;
        results[t].cross_domain_accesses = total_cross;
        results[t].coherency_requests = stats.coherency_requests;
        results[t].evictions = stats.evictions;
        results[t].throughput = (elapsed_ns > 0)
            ? (double)total_visited / ((double)elapsed_ns / 1e9)
            : 0.0;
    }

    /* Clean up nodes from last trial */
    free_host_nodes();

    return 0;
}

/* ------------------------------------------------------------------ */
/* Hash table sub-experiment                                           */
/* ------------------------------------------------------------------ */

typedef struct HTNode {
    uint64_t key;
    uint64_t value;
    uint64_t next_handle;  /* same encoding as graph handles */
} HTNode;

typedef struct {
    double   time_ms;
    uint64_t cross_domain;
    uint64_t coherency_requests;
} HTResult;

/* Hash function */
static inline uint64_t ht_hash(uint64_t key)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return key;
}

static int run_hash_table_experiment(const char *label, int ht_policy,
                                     HTResult *result)
{
    (void)label;

    /* Buckets always in device coherent memory */
    uint64_t bucket_pool_size = HT_NUM_BUCKETS * sizeof(uint64_t);
    void *bucket_pool = NULL;
    int ret = cxlCoherentAlloc(bucket_pool_size, &bucket_pool);
    if (ret != CUDA_SUCCESS || !bucket_pool) {
        fprintf(stderr, "  HT: bucket alloc failed\n");
        return -1;
    }
    uint64_t *buckets = (uint64_t *)bucket_pool;
    memset(buckets, 0, bucket_pool_size);

    /* Chain node storage */
    int max_chain_nodes = HT_NUM_OPS; /* worst case: all inserts */
    HTNode **chain_ptrs = (HTNode **)calloc(max_chain_nodes, sizeof(HTNode *));
    uint64_t *chain_handles = (uint64_t *)calloc(max_chain_nodes, sizeof(uint64_t));
    int *chain_pool_type = (int *)calloc(max_chain_nodes, sizeof(int));
    if (!chain_ptrs || !chain_handles || !chain_pool_type) {
        cxlCoherentFree(bucket_pool);
        free(chain_ptrs); free(chain_handles); free(chain_pool_type);
        return -1;
    }

    /* Device pool for chain nodes (if needed) */
    uint64_t chain_dev_size = (uint64_t)max_chain_nodes * sizeof(HTNode);
    void *chain_dev_pool = NULL;
    ret = cxlCoherentAlloc(chain_dev_size, &chain_dev_pool);
    if (ret != CUDA_SUCCESS || !chain_dev_pool) {
        fprintf(stderr, "  HT: chain dev alloc failed\n");
        cxlCoherentFree(bucket_pool);
        free(chain_ptrs); free(chain_handles); free(chain_pool_type);
        return -1;
    }
    cxlSetBias(chain_dev_pool, chain_dev_size, CXL_BIAS_DEVICE);
    uint64_t chain_dev_used = 0;
    int chain_count = 0;

    uint64_t cross_domain = 0;

    cxlResetCoherencyStats();
    cxlCoherentFence();

    uint64_t t_start = time_ns();

    for (int op = 0; op < HT_NUM_OPS; op++) {
        uint64_t key = rng_next();
        int is_put = (rng_int(2) == 0);
        uint64_t bucket_idx = ht_hash(key) % HT_NUM_BUCKETS;

        if (is_put) {
            /* Allocate chain node */
            int pool;
            switch (ht_policy) {
            case 0: /* Random */
                pool = rng_int(2);
                break;
            case 1: /* Round-robin */
                pool = (chain_count % 2);
                break;
            case 2: /* Affinity: chain node near bucket (device) */
                pool = POOL_DEVICE;
                break;
            default:
                pool = POOL_HOST;
                break;
            }

            HTNode *new_node = NULL;
            uint64_t new_handle = HANDLE_NULL;

            if (pool == POOL_DEVICE) {
                uint64_t aligned_sz = (sizeof(HTNode) + 63) & ~63ULL;
                if (chain_dev_used + aligned_sz <= chain_dev_size) {
                    new_node = (HTNode *)((uint8_t *)chain_dev_pool + chain_dev_used);
                    new_handle = make_device_handle(chain_dev_used);
                    /* Re-encode: offset relative to chain_dev_pool, but
                       our resolve_handle uses g_dev_pool.  We need a separate
                       scheme for the HT.  Use direct host-accessible pointers
                       with the host-bit scheme, since chain_dev_pool is also
                       CPU-accessible (it's a BAR4 mapping). */
                    new_handle = make_host_handle(new_node);
                    chain_dev_used += aligned_sz;
                } else {
                    /* Fall back to host */
                    pool = POOL_HOST;
                }
            }

            if (pool == POOL_HOST) {
                new_node = (HTNode *)malloc(sizeof(HTNode));
                if (!new_node) continue;
                new_handle = make_host_handle(new_node);
            }

            new_node->key = key;
            new_node->value = op;
            new_node->next_handle = buckets[bucket_idx];
            buckets[bucket_idx] = new_handle;

            chain_ptrs[chain_count] = new_node;
            chain_handles[chain_count] = new_handle;
            chain_pool_type[chain_count] = pool;
            chain_count++;

            /* Cross-domain: bucket is in device, chain node might be host */
            if (pool == POOL_HOST) cross_domain++;
        } else {
            /* GET: traverse chain */
            uint64_t h = buckets[bucket_idx];
            int prev_pool = POOL_DEVICE; /* bucket is in device memory */
            while (h != HANDLE_NULL) {
                HTNode *node;
                int node_pool;
                if (h & HANDLE_HOST_BIT) {
                    node = (HTNode *)(uintptr_t)(h & ~HANDLE_HOST_BIT);
                    /* Determine if this pointer is in chain_dev_pool range */
                    if ((uintptr_t)node >= (uintptr_t)chain_dev_pool &&
                        (uintptr_t)node < (uintptr_t)chain_dev_pool + chain_dev_size) {
                        node_pool = POOL_DEVICE;
                    } else {
                        node_pool = POOL_HOST;
                    }
                } else {
                    node = (HTNode *)resolve_handle(h);
                    node_pool = POOL_DEVICE;
                }
                if (!node) break;

                if (node_pool != prev_pool) cross_domain++;
                prev_pool = node_pool;

                if (node->key == key) break;
                h = node->next_handle;
            }
        }
    }

    uint64_t t_end = time_ns();

    CXLCoherencyStats stats;
    cxlGetCoherencyStats(&stats);

    result->time_ms = ns_to_ms(t_end - t_start);
    result->cross_domain = cross_domain;
    result->coherency_requests = stats.coherency_requests;

    /* Cleanup chain nodes */
    for (int i = 0; i < chain_count; i++) {
        if (chain_pool_type[i] == POOL_HOST) {
            /* Only free if it's actually in host DRAM, not BAR4 */
            HTNode *p = chain_ptrs[i];
            if ((uintptr_t)p < (uintptr_t)chain_dev_pool ||
                (uintptr_t)p >= (uintptr_t)chain_dev_pool + chain_dev_size) {
                free(p);
            }
        }
    }
    cxlCoherentFree(chain_dev_pool);
    cxlCoherentFree(bucket_pool);
    free(chain_ptrs);
    free(chain_handles);
    free(chain_pool_type);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Access skew analysis                                                */
/* ------------------------------------------------------------------ */

static void access_skew_analysis(void)
{
    printf("\n=== Access Skew Analysis ===\n");

    /* Collect per-node access and cross-domain counts from last run */
    uint64_t *cross_counts = (uint64_t *)malloc(g_num_nodes * sizeof(uint64_t));
    uint64_t *acc_counts   = (uint64_t *)malloc(g_num_nodes * sizeof(uint64_t));
    if (!cross_counts || !acc_counts) {
        free(cross_counts); free(acc_counts);
        printf("  (skipped: allocation failed)\n");
        return;
    }

    uint64_t total_cross = 0;
    uint64_t total_access = 0;
    for (int i = 0; i < g_num_nodes; i++) {
        cross_counts[i] = g_meta[i].cross_domain_cnt;
        acc_counts[i]   = g_meta[i].access_count;
        total_cross  += cross_counts[i];
        total_access += acc_counts[i];
    }

    /* Sort descending */
    qsort(cross_counts, g_num_nodes, sizeof(uint64_t), cmp_access_desc);
    qsort(acc_counts,   g_num_nodes, sizeof(uint64_t), cmp_access_desc);

    printf("  Total cross-domain accesses: %lu\n", (unsigned long)total_cross);
    printf("  Total node accesses:         %lu\n", (unsigned long)total_access);

    if (total_cross > 0) {
        printf("\n  Cross-domain traffic concentration:\n");
        int pcts[] = {1, 5, 10, 20, 50};
        for (int p = 0; p < 5; p++) {
            int count = (g_num_nodes * pcts[p] + 99) / 100;
            uint64_t sum = 0;
            for (int i = 0; i < count && i < g_num_nodes; i++)
                sum += cross_counts[i];
            printf("    Top %2d%% nodes (%4d nodes) -> %5.1f%% of cross-domain traffic\n",
                   pcts[p], count,
                   (double)sum * 100.0 / (double)total_cross);
        }
    }

    if (total_access > 0) {
        printf("\n  Access frequency concentration:\n");
        int pcts[] = {1, 5, 10, 20, 50};
        for (int p = 0; p < 5; p++) {
            int count = (g_num_nodes * pcts[p] + 99) / 100;
            uint64_t sum = 0;
            for (int i = 0; i < count && i < g_num_nodes; i++)
                sum += acc_counts[i];
            printf("    Top %2d%% nodes (%4d nodes) -> %5.1f%% of total accesses\n",
                   pcts[p], count,
                   (double)sum * 100.0 / (double)total_access);
        }
    }

    free(cross_counts);
    free(acc_counts);
}

/* ------------------------------------------------------------------ */
/* Median helper                                                       */
/* ------------------------------------------------------------------ */

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static double median_double(double *arr, int n)
{
    double *tmp = (double *)malloc(n * sizeof(double));
    if (!tmp) return arr[0];
    memcpy(tmp, arr, n * sizeof(double));
    qsort(tmp, n, sizeof(double), cmp_double);
    double m = (n % 2) ? tmp[n / 2] : (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0;
    free(tmp);
    return m;
}

static uint64_t median_u64(uint64_t *arr, int n)
{
    uint64_t *tmp = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (!tmp) return arr[0];
    memcpy(tmp, arr, n * sizeof(uint64_t));
    qsort(tmp, n, sizeof(uint64_t), cmp_access_desc); /* descending, but median same */
    /* Actually need ascending for proper median */
    /* Just pick middle element after sorting -- order doesn't matter for median */
    uint64_t m = (n % 2) ? tmp[n / 2] : (tmp[n / 2 - 1] + tmp[n / 2]) / 2;
    free(tmp);
    return m;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    printf("=== RQ4: Allocation Policy vs. Coherence Traffic ===\n\n");

    /* Allow overriding node count via argv[1] */
    g_num_nodes = DEFAULT_NUM_NODES;
    if (argc > 1) {
        int n = atoi(argv[1]);
        if (n > 0 && n <= 100000) g_num_nodes = n;
    }

    printf("Configuration:\n");
    printf("  Nodes:         %d\n", g_num_nodes);
    printf("  Avg degree:    %d\n", AVG_DEGREE);
    printf("  Node size:     %zu bytes (1 cache line)\n", sizeof(GraphNode));
    printf("  BFS roots:     %d per trial\n", NUM_BFS_ROOTS);
    printf("  Trials:        %d\n", NUM_TRIALS);
    printf("  Migration K:   %d nodes/round\n", MIGRATE_TOP_K);

    /* Initialize CUDA/CXL */
    CUresult err = cuInit(0);
    if (err != CUDA_SUCCESS) {
        fprintf(stderr, "cuInit failed: %d\n", err);
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

    /* Allocate meta array and BFS queue */
    g_meta = (NodeMeta *)calloc(g_num_nodes, sizeof(NodeMeta));
    g_bfs_queue = (int *)malloc(g_num_nodes * sizeof(int));
    if (!g_meta || !g_bfs_queue) {
        fprintf(stderr, "Failed to allocate meta/queue\n");
        return 1;
    }

    /* Initialize device memory pool */
    if (dev_pool_init() != 0) {
        fprintf(stderr, "Failed to init device pool\n");
        free(g_meta);
        free(g_bfs_queue);
        return 1;
    }

    /* ============================================================== */
    /* Part 1: Graph BFS experiments                                   */
    /* ============================================================== */

    printf("\n========================================\n");
    printf("Part 1: Graph BFS Experiments\n");
    printf("========================================\n");

    const char *policy_names[] = {
        "Random",
        "Static Affinity (BFS proximity)",
        "Topology-Aware (BFS bisection)",
        "Online Migration"
    };
    int num_policies = 4;

    TrialResult all_results[4][NUM_TRIALS];

    for (int p = 0; p < num_policies; p++) {
        dev_pool_reset();
        if (run_bfs_experiment(policy_names[p], p,
                               all_results[p], NUM_TRIALS) != 0) {
            fprintf(stderr, "Policy %d (%s) failed\n", p, policy_names[p]);
            /* Continue with other policies */
        }
    }

    /* Access skew analysis: run one more BFS with random policy to collect data */
    printf("\n========================================\n");
    printf("Access Skew Analysis (Random policy, single run)\n");
    printf("========================================\n");
    {
        dev_pool_reset();
        free_host_nodes();
        memset(g_meta, 0, g_num_nodes * sizeof(NodeMeta));
        rng_seed(0xAAAABBBBCCCCDDDDULL);
        if (policy_random() == 0) {
            wire_edges();
            cxlCoherentFence();
            for (int i = 0; i < g_num_nodes; i++) {
                g_meta[i].access_count = 0;
                g_meta[i].cross_domain_cnt = 0;
            }
            for (int r = 0; r < NUM_BFS_ROOTS; r++) {
                uint64_t cross;
                int root = (r * (g_num_nodes / NUM_BFS_ROOTS)) % g_num_nodes;
                bfs_run(root, &cross);
            }
            access_skew_analysis();
        }
        free_host_nodes();
    }

    /* ============================================================== */
    /* Part 2: Summary table                                           */
    /* ============================================================== */

    printf("\n========================================\n");
    printf("Part 1 Summary: Graph BFS (medians over %d trials)\n", NUM_TRIALS);
    printf("========================================\n");
    printf("%-35s %10s %10s %10s %12s %15s\n",
           "Policy", "Time(ms)", "Cross-Dom", "Coh.Reqs", "Evictions",
           "Throughput(N/s)");
    printf("%-35s %10s %10s %10s %12s %15s\n",
           "-----------------------------------", "----------", "----------",
           "----------", "------------", "---------------");

    double baseline_cross = 0;
    double baseline_time = 0;

    for (int p = 0; p < num_policies; p++) {
        double times[NUM_TRIALS];
        uint64_t cross[NUM_TRIALS], coh[NUM_TRIALS], evict[NUM_TRIALS];
        double tput[NUM_TRIALS];

        for (int t = 0; t < NUM_TRIALS; t++) {
            times[t] = all_results[p][t].total_time_ms;
            cross[t] = all_results[p][t].cross_domain_accesses;
            coh[t]   = all_results[p][t].coherency_requests;
            evict[t] = all_results[p][t].evictions;
            tput[t]  = all_results[p][t].throughput;
        }

        double med_time  = median_double(times, NUM_TRIALS);
        uint64_t med_cross = median_u64(cross, NUM_TRIALS);
        uint64_t med_coh   = median_u64(coh, NUM_TRIALS);
        uint64_t med_evict = median_u64(evict, NUM_TRIALS);
        double med_tput  = median_double(tput, NUM_TRIALS);

        if (p == 0) {
            baseline_cross = (double)med_cross;
            baseline_time  = med_time;
        }

        printf("%-35s %10.2f %10lu %10lu %12lu %15.0f\n",
               policy_names[p],
               med_time,
               (unsigned long)med_cross,
               (unsigned long)med_coh,
               (unsigned long)med_evict,
               med_tput);
    }

    /* Traffic reduction and throughput improvement */
    if (baseline_cross > 0 || baseline_time > 0) {
        printf("\n%-35s %10s %15s\n",
               "Policy", "Traffic%", "Speedup");
        printf("%-35s %10s %15s\n",
               "-----------------------------------", "----------",
               "---------------");

        for (int p = 0; p < num_policies; p++) {
            double times[NUM_TRIALS];
            uint64_t cross[NUM_TRIALS];
            for (int t = 0; t < NUM_TRIALS; t++) {
                times[t] = all_results[p][t].total_time_ms;
                cross[t] = all_results[p][t].cross_domain_accesses;
            }
            double med_time  = median_double(times, NUM_TRIALS);
            uint64_t med_cross = median_u64(cross, NUM_TRIALS);

            double traffic_reduction = (baseline_cross > 0)
                ? (1.0 - (double)med_cross / baseline_cross) * 100.0
                : 0.0;
            double speedup = (med_time > 0 && baseline_time > 0)
                ? baseline_time / med_time
                : 1.0;

            printf("%-35s %+9.1f%% %14.2fx\n",
                   policy_names[p], traffic_reduction, speedup);
        }
    }

    /* ============================================================== */
    /* Part 3: Hash table sub-experiment                               */
    /* ============================================================== */

    printf("\n========================================\n");
    printf("Part 2: Hash Table Sub-experiment\n");
    printf("========================================\n");
    printf("  Buckets: %d (device coherent memory)\n", HT_NUM_BUCKETS);
    printf("  Ops:     %d (50%% get, 50%% put)\n", HT_NUM_OPS);

    const char *ht_policy_names[] = {
        "Random",
        "Round-robin (alternate)",
        "Affinity (chain near bucket)"
    };
    int ht_num_policies = 3;

    printf("\n%-30s %10s %12s %12s\n",
           "HT Policy", "Time(ms)", "Cross-Dom", "Coh.Reqs");
    printf("%-30s %10s %12s %12s\n",
           "------------------------------", "----------",
           "------------", "------------");

    for (int hp = 0; hp < ht_num_policies; hp++) {
        HTResult ht_results[NUM_TRIALS];
        double ht_times[NUM_TRIALS];
        uint64_t ht_cross[NUM_TRIALS], ht_coh[NUM_TRIALS];

        for (int t = 0; t < NUM_TRIALS; t++) {
            rng_seed((uint64_t)(t + 1) * 0xBEEF + (uint64_t)hp * 0x9999);
            if (run_hash_table_experiment(ht_policy_names[hp], hp,
                                          &ht_results[t]) != 0) {
                ht_results[t].time_ms = 0;
                ht_results[t].cross_domain = 0;
                ht_results[t].coherency_requests = 0;
            }
            ht_times[t] = ht_results[t].time_ms;
            ht_cross[t] = ht_results[t].cross_domain;
            ht_coh[t]   = ht_results[t].coherency_requests;
        }

        double med_time = median_double(ht_times, NUM_TRIALS);
        uint64_t med_cross = median_u64(ht_cross, NUM_TRIALS);
        uint64_t med_coh   = median_u64(ht_coh, NUM_TRIALS);

        printf("%-30s %10.2f %12lu %12lu\n",
               ht_policy_names[hp],
               med_time,
               (unsigned long)med_cross,
               (unsigned long)med_coh);
    }

    /* ============================================================== */
    /* Cleanup                                                         */
    /* ============================================================== */

    dev_pool_destroy();
    free(g_meta);
    free(g_bfs_queue);

    printf("\n=== RQ4 experiment complete ===\n");
    return 0;
}
