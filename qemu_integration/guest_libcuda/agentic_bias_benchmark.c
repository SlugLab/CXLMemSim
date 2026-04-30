/*
 * Agentic workload bias-mode benchmark for CXL Type-2.
 *
 * Models an agent loop with four hot memory structures:
 *   - retrieval index nodes (device-friendly read/update stream)
 *   - KV/history entries (device-friendly append/reuse)
 *   - planner scratch state (host-friendly control state)
 *   - tool/task queue (host-friendly producer/consumer metadata)
 *
 * Build for CXL guest:
 *   gcc -O2 -Wall -o agentic_bias_benchmark agentic_bias_benchmark.c -L. -lcuda -lm -Wl,-rpath,.
 *
 * Build native host baseline:
 *   gcc -O2 -Wall -DCXL_AGENTIC_NATIVE -o agentic_bias_benchmark_native agentic_bias_benchmark.c -lm
 */

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cxl_gpu_cmd.h"

#define CACHE_LINE 64
#define DEFAULT_STEPS 20000
#define DEFAULT_TRIALS 5
#define DEFAULT_INDEX_ENTRIES 16384
#define DEFAULT_KV_ENTRIES 8192
#define DEFAULT_QUEUE_ENTRIES 1024
#define DEFAULT_SCRATCH_ENTRIES 1024
#define RETRIEVAL_PROBES 32
#define KV_REUSE_WINDOW 16

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;

#define CUDA_SUCCESS 0

typedef struct AgentIndexNode {
    uint64_t id;
    uint64_t vector_tag;
    uint64_t score;
    uint64_t access_count;
    uint64_t next_hint;
    uint64_t owner;
    uint8_t pad[16];
} AgentIndexNode;

typedef struct AgentKVEntry {
    uint64_t token_id;
    uint64_t key_hash;
    uint64_t value_hash;
    uint64_t age;
    uint64_t reuse_count;
    uint64_t flags;
    uint8_t pad[16];
} AgentKVEntry;

typedef struct AgentTask {
    uint64_t task_id;
    uint64_t tool_id;
    uint64_t state;
    uint64_t payload_hash;
    uint64_t dependency;
    uint64_t result_hash;
    uint8_t pad[16];
} AgentTask;

typedef struct AgentScratch {
    uint64_t step_id;
    uint64_t plan_hash;
    uint64_t branch_score;
    uint64_t tool_mask;
    uint64_t budget;
    uint64_t checksum;
    uint8_t pad[16];
} AgentScratch;

typedef struct CXLCoherencyStats {
    uint64_t snoop_hits;
    uint64_t snoop_misses;
    uint64_t coherency_requests;
    uint64_t back_invalidations;
    uint64_t writebacks;
    uint64_t evictions;
    uint64_t bias_flips;
    uint64_t device_bias_hits;
    uint64_t host_bias_hits;
    uint64_t upgrades;
    uint64_t downgrades;
    uint64_t directory_entries;
} CXLCoherencyStats;

#ifndef CXL_AGENTIC_NATIVE
extern CUresult cuInit(unsigned int flags);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev);
extern int cxlCoherentAlloc(uint64_t size, void **host_ptr);
extern int cxlCoherentFree(void *host_ptr);
extern int cxlCoherentFence(void);
extern int cxlSetBias(void *host_ptr, uint64_t size, int bias_mode);
extern int cxlBiasFlip(void *host_ptr, uint64_t size, int new_bias);
extern int cxlGetCoherencyStats(CXLCoherencyStats *stats);
extern int cxlResetCoherencyStats(void);
#else
static CUresult cuInit(unsigned int flags) { (void)flags; return CUDA_SUCCESS; }
static CUresult cuDeviceGet(CUdevice *device, int ordinal)
{
    (void)ordinal;
    if (device) *device = 0;
    return CUDA_SUCCESS;
}
static CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev)
{
    (void)flags; (void)dev;
    if (ctx) *ctx = (void *)1;
    return CUDA_SUCCESS;
}
static int cxlCoherentAlloc(uint64_t size, void **host_ptr)
{
    return posix_memalign(host_ptr, CACHE_LINE, (size_t)size);
}
static int cxlCoherentFree(void *host_ptr) { free(host_ptr); return 0; }
static int cxlCoherentFence(void) { __sync_synchronize(); return 0; }
static int cxlSetBias(void *host_ptr, uint64_t size, int bias_mode)
{
    (void)host_ptr; (void)size; (void)bias_mode; return 0;
}
static int cxlBiasFlip(void *host_ptr, uint64_t size, int new_bias)
{
    (void)host_ptr; (void)size; (void)new_bias; return 0;
}
static int cxlGetCoherencyStats(CXLCoherencyStats *stats)
{
    if (stats) memset(stats, 0, sizeof(*stats));
    return 0;
}
static int cxlResetCoherencyStats(void) { return 0; }
#endif

typedef struct AgentRegions {
    AgentIndexNode *index;
    AgentKVEntry *kv;
    AgentTask *queue;
    AgentScratch *scratch;
    uint64_t index_entries;
    uint64_t kv_entries;
    uint64_t queue_entries;
    uint64_t scratch_entries;
} AgentRegions;

typedef struct ModeConfig {
    const char *name;
    int index_bias;
    int kv_bias;
    int queue_bias;
    int scratch_bias;
    int phase_flip;
} ModeConfig;

static const ModeConfig modes[] = {
    { "host-bias", CXL_BIAS_HOST, CXL_BIAS_HOST, CXL_BIAS_HOST, CXL_BIAS_HOST, 0 },
    { "device-bias", CXL_BIAS_DEVICE, CXL_BIAS_DEVICE, CXL_BIAS_DEVICE, CXL_BIAS_DEVICE, 0 },
    { "hybrid-agentic", CXL_BIAS_DEVICE, CXL_BIAS_DEVICE, CXL_BIAS_HOST, CXL_BIAS_HOST, 0 },
    { "phase-flip", CXL_BIAS_HOST, CXL_BIAS_HOST, CXL_BIAS_HOST, CXL_BIAS_HOST, 1 },
};

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t xorshift64(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t av = *(const uint64_t *)a;
    uint64_t bv = *(const uint64_t *)b;
    return (av > bv) - (av < bv);
}

static uint64_t parse_u64_arg(const char *arg, uint64_t fallback)
{
    char *end = NULL;
    unsigned long long value;

    if (!arg || !*arg) return fallback;
    errno = 0;
    value = strtoull(arg, &end, 10);
    if (errno || end == arg || *end != '\0' || value == 0) return fallback;
    return (uint64_t)value;
}

static int encoded_bias(int mode, uint64_t granularity)
{
    if (granularity == CXL_BIAS_GRAN_FLIT) {
        return mode;
    }
    return (int)CXL_BIAS_ENCODE(mode, granularity);
}

static void cxl_zero(volatile void *ptr, size_t size)
{
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    for (size_t i = 0; i < size; i++) {
        p[i] = 0;
    }
}

static int alloc_region(void **ptr, uint64_t count, size_t elem_size)
{
    uint64_t bytes = count * (uint64_t)elem_size;
    int rc = cxlCoherentAlloc(bytes, ptr);
    if (rc != 0 || !*ptr) {
        return -1;
    }
    cxl_zero(*ptr, (size_t)bytes);
    return 0;
}

static int alloc_regions(AgentRegions *r, uint64_t index_entries, uint64_t kv_entries,
                         uint64_t queue_entries, uint64_t scratch_entries)
{
    memset(r, 0, sizeof(*r));
    r->index_entries = index_entries;
    r->kv_entries = kv_entries;
    r->queue_entries = queue_entries;
    r->scratch_entries = scratch_entries;

    if (alloc_region((void **)&r->index, index_entries, sizeof(*r->index)) != 0 ||
        alloc_region((void **)&r->kv, kv_entries, sizeof(*r->kv)) != 0 ||
        alloc_region((void **)&r->queue, queue_entries, sizeof(*r->queue)) != 0 ||
        alloc_region((void **)&r->scratch, scratch_entries, sizeof(*r->scratch)) != 0) {
        return -1;
    }
    return 0;
}

static void free_regions(AgentRegions *r)
{
    if (r->index) cxlCoherentFree(r->index);
    if (r->kv) cxlCoherentFree(r->kv);
    if (r->queue) cxlCoherentFree(r->queue);
    if (r->scratch) cxlCoherentFree(r->scratch);
}

static void initialize_regions(AgentRegions *r)
{
    for (uint64_t i = 0; i < r->index_entries; i++) {
        r->index[i].id = i;
        r->index[i].vector_tag = i * 1315423911ULL;
        r->index[i].score = (i ^ 0xa5a5a5a5ULL) & 0xffff;
        r->index[i].next_hint = (i * 17 + 23) % r->index_entries;
    }
    for (uint64_t i = 0; i < r->kv_entries; i++) {
        r->kv[i].token_id = i;
        r->kv[i].key_hash = i * 2654435761ULL;
        r->kv[i].value_hash = i ^ 0x123456789abcdef0ULL;
    }
    cxlCoherentFence();
}

static int apply_mode(const AgentRegions *r, const ModeConfig *mode, uint64_t granularity)
{
    if (cxlSetBias(r->index, r->index_entries * sizeof(*r->index),
                   encoded_bias(mode->index_bias, granularity)) != 0) return -1;
    if (cxlSetBias(r->kv, r->kv_entries * sizeof(*r->kv),
                   encoded_bias(mode->kv_bias, granularity)) != 0) return -1;
    if (cxlSetBias(r->queue, r->queue_entries * sizeof(*r->queue),
                   encoded_bias(mode->queue_bias, granularity)) != 0) return -1;
    if (cxlSetBias(r->scratch, r->scratch_entries * sizeof(*r->scratch),
                   encoded_bias(mode->scratch_bias, granularity)) != 0) return -1;
    cxlCoherentFence();
    return 0;
}

static int flip_for_phase(const AgentRegions *r, uint64_t granularity, int retrieval_phase)
{
    int device = encoded_bias(CXL_BIAS_DEVICE, granularity);
    int host = encoded_bias(CXL_BIAS_HOST, granularity);

    if (retrieval_phase) {
        if (cxlBiasFlip(r->index, r->index_entries * sizeof(*r->index), device) != 0) return -1;
        if (cxlBiasFlip(r->kv, r->kv_entries * sizeof(*r->kv), device) != 0) return -1;
    } else {
        if (cxlBiasFlip(r->queue, r->queue_entries * sizeof(*r->queue), host) != 0) return -1;
        if (cxlBiasFlip(r->scratch, r->scratch_entries * sizeof(*r->scratch), host) != 0) return -1;
    }
    return 0;
}

static uint64_t run_agent_loop(AgentRegions *r, uint64_t steps, const ModeConfig *mode,
                               uint64_t granularity)
{
    uint64_t checksum = 0;

    rng_state = 0xfeedface12345678ULL;
    for (uint64_t step = 0; step < steps; step++) {
        AgentScratch *scratch = &r->scratch[step % r->scratch_entries];
        AgentTask *task = &r->queue[step % r->queue_entries];

        if (mode->phase_flip && (step % 64) == 0) {
            flip_for_phase(r, granularity, 0);
        }

        scratch->step_id = step;
        scratch->budget = 4096 - (step & 1023);
        scratch->tool_mask ^= (1ULL << (step % 17));
        scratch->plan_hash = (scratch->plan_hash * 1315423911ULL) ^ step;

        task->task_id = step;
        task->tool_id = step % 11;
        task->state = 1;
        task->payload_hash = scratch->plan_hash ^ 0xd1b54a32d192ed03ULL;

        if (mode->phase_flip && (step % 64) == 0) {
            flip_for_phase(r, granularity, 1);
        }

        uint64_t idx = xorshift64() % r->index_entries;
        for (int probe = 0; probe < RETRIEVAL_PROBES; probe++) {
            AgentIndexNode *node = &r->index[idx];
            checksum += node->score ^ node->vector_tag ^ node->access_count;
            node->access_count++;
            node->score = (node->score + scratch->budget + probe) & 0xffffffffULL;
            idx = (node->next_hint + xorshift64()) % r->index_entries;
        }

        uint64_t kv_pos = step % r->kv_entries;
        AgentKVEntry *entry = &r->kv[kv_pos];
        entry->token_id = step;
        entry->key_hash = checksum ^ step;
        entry->value_hash = checksum + 0x9e3779b97f4a7c15ULL;
        entry->age = step;
        entry->reuse_count++;

        for (uint64_t k = 1; k <= KV_REUSE_WINDOW; k++) {
            AgentKVEntry *old = &r->kv[(kv_pos + r->kv_entries - k) % r->kv_entries];
            checksum ^= old->key_hash + old->value_hash + old->reuse_count;
            old->reuse_count++;
        }

        task->result_hash = checksum ^ task->payload_hash;
        task->state = 2;
        scratch->checksum ^= task->result_hash;
        checksum += scratch->checksum;
    }

    cxlCoherentFence();
    return checksum;
}

static void print_stats(const CXLCoherencyStats *s)
{
    printf("  coherency_requests=%" PRIu64 " back_invalidations=%" PRIu64
           " writebacks=%" PRIu64 " bias_flips=%" PRIu64 "\n",
           s->coherency_requests, s->back_invalidations, s->writebacks, s->bias_flips);
    printf("  snoop_hits=%" PRIu64 " snoop_misses=%" PRIu64
           " host_bias_hits=%" PRIu64 " device_bias_hits=%" PRIu64
           " directory_entries=%" PRIu64 "\n",
           s->snoop_hits, s->snoop_misses, s->host_bias_hits,
           s->device_bias_hits, s->directory_entries);
}

int main(int argc, char **argv)
{
    uint64_t steps = parse_u64_arg(argc > 1 ? argv[1] : NULL, DEFAULT_STEPS);
    uint64_t trials = parse_u64_arg(argc > 2 ? argv[2] : NULL, DEFAULT_TRIALS);
    uint64_t granularity = parse_u64_arg(argc > 3 ? argv[3] : NULL, CXL_BIAS_GRAN_FLIT);
    uint64_t index_entries = parse_u64_arg(argc > 4 ? argv[4] : NULL, DEFAULT_INDEX_ENTRIES);
    uint64_t kv_entries = parse_u64_arg(argc > 5 ? argv[5] : NULL, DEFAULT_KV_ENTRIES);
    AgentRegions regions;
    CUdevice dev;
    CUcontext ctx;

    if (sizeof(AgentIndexNode) != CACHE_LINE || sizeof(AgentKVEntry) != CACHE_LINE ||
        sizeof(AgentTask) != CACHE_LINE || sizeof(AgentScratch) != CACHE_LINE) {
        fprintf(stderr, "agent records must be 64-byte cache lines\n");
        return 1;
    }

    if (cuInit(0) != CUDA_SUCCESS || cuDeviceGet(&dev, 0) != CUDA_SUCCESS ||
        cuCtxCreate_v2(&ctx, 0, dev) != CUDA_SUCCESS) {
        fprintf(stderr, "failed to initialize CUDA/CXL context\n");
        return 1;
    }

    if (alloc_regions(&regions, index_entries, kv_entries,
                      DEFAULT_QUEUE_ENTRIES, DEFAULT_SCRATCH_ENTRIES) != 0) {
        fprintf(stderr, "failed to allocate agentic regions\n");
        return 1;
    }
    initialize_regions(&regions);

    printf("Agentic CXL Type-2 bias benchmark\n");
    printf("  backend=%s steps=%" PRIu64 " trials=%" PRIu64
           " bias_granularity=%" PRIu64 " bytes\n",
#ifdef CXL_AGENTIC_NATIVE
           "native",
#else
           "cxl-type2",
#endif
           steps, trials, granularity);
    printf("  index_entries=%" PRIu64 " kv_entries=%" PRIu64
           " queue_entries=%" PRIu64 " scratch_entries=%" PRIu64 "\n",
           regions.index_entries, regions.kv_entries,
           regions.queue_entries, regions.scratch_entries);

    for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        uint64_t samples[32];
        uint64_t checksum = 0;
        CXLCoherencyStats stats;
        uint64_t n = trials > 32 ? 32 : trials;

        initialize_regions(&regions);
        if (apply_mode(&regions, &modes[m], granularity) != 0) {
            fprintf(stderr, "failed to apply bias mode %s\n", modes[m].name);
            free_regions(&regions);
            return 1;
        }
        cxlResetCoherencyStats();

        for (uint64_t t = 0; t < n; t++) {
            uint64_t start = now_ns();
            checksum ^= run_agent_loop(&regions, steps, &modes[m], granularity);
            samples[t] = now_ns() - start;
        }
        qsort(samples, (size_t)n, sizeof(samples[0]), cmp_u64);
        memset(&stats, 0, sizeof(stats));
        cxlGetCoherencyStats(&stats);

        uint64_t median_ns = samples[n / 2];
        double agent_steps_per_s = median_ns ? (double)steps * 1.0e9 / (double)median_ns : 0.0;

        printf("\n[%s]\n", modes[m].name);
        printf("  median_ns=%" PRIu64 " p25_ns=%" PRIu64 " p75_ns=%" PRIu64 "\n",
               median_ns, samples[n / 4], samples[(3 * n) / 4]);
        printf("  agent_steps_per_sec=%.2f checksum=%" PRIu64 "\n",
               agent_steps_per_s, checksum);
        print_stats(&stats);
    }

    free_regions(&regions);
    return 0;
}
