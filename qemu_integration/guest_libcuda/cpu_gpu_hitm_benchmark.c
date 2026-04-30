/*
 * CPU/GPU HITM-style coherency benchmark for the CXL Type-2 guest stack.
 *
 * The benchmark allocates one or more cache lines from the BAR4 coherent pool,
 * has the CPU touch each line, then launches a GPU kernel that reads and
 * updates the same lines. This gives a serialized ownership handoff similar in
 * spirit to MLC HITM, but across CPU and GPU instead of two CPU sockets.
 *
 * Usage:
 *   LD_LIBRARY_PATH=. ./cpu_gpu_hitm_benchmark [lines] [iterations] [warmup]
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cxl_gpu_cmd.h"

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef void *CUmodule;
typedef void *CUfunction;
typedef uint64_t CUdeviceptr;

#define CUDA_SUCCESS 0

extern CUresult cuInit(unsigned int flags);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev);
extern CUresult cuCtxDestroy_v2(CUcontext ctx);
extern CUresult cuCtxSynchronize(void);
extern CUresult cuModuleLoadData(CUmodule *module, const void *image);
extern CUresult cuModuleGetFunction(CUfunction *hfunc, CUmodule hmod, const char *name);
extern CUresult cuLaunchKernel(CUfunction f,
                               unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
                               unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
                               unsigned int sharedMemBytes, void *hStream,
                               void **kernelParams, void **extra);

extern int cxlCoherentAlloc(uint64_t size, void **host_ptr);
extern int cxlCoherentFree(void *host_ptr);
extern int cxlCoherentFence(void);
extern int cxlSetBias(void *host_ptr, uint64_t size, int bias_mode);
extern int cxlBiasFlip(void *host_ptr, uint64_t size, int new_bias);
extern int cxlGetCoherencyStats(void *stats);
extern int cxlResetCoherencyStats(void);

typedef struct {
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

typedef struct __attribute__((aligned(64))) {
    volatile uint32_t counter;
    volatile uint32_t gpu_seq;
    volatile uint32_t cpu_seq;
    volatile uint32_t checksum;
    uint8_t pad[48];
} SharedLine;

typedef struct {
    const char *label;
    int initial_bias;
    int flip_each_iteration;
} BenchmarkMode;

static const char *hitm_ptx =
    ".version 8.0\n"
    ".target sm_90\n"
    ".address_size 64\n"
    "\n"
    ".visible .entry gpu_hitm_touch(\n"
    "    .param .u64 base,\n"
    "    .param .u32 lines,\n"
    "    .param .u32 seq\n"
    ")\n"
    "{\n"
    "    .reg .pred %p<2>;\n"
    "    .reg .b32 %r<8>;\n"
    "    .reg .b64 %rd<8>;\n"
    "\n"
    "    ld.param.u64 %rd1, [base];\n"
    "    ld.param.u32 %r1, [lines];\n"
    "    ld.param.u32 %r2, [seq];\n"
    "\n"
    "    mov.u32 %r3, %ctaid.x;\n"
    "    mov.u32 %r4, %ntid.x;\n"
    "    mov.u32 %r5, %tid.x;\n"
    "    mad.lo.u32 %r6, %r3, %r4, %r5;\n"
    "    setp.ge.u32 %p1, %r6, %r1;\n"
    "    @%p1 bra DONE;\n"
    "\n"
    "    mul.wide.u32 %rd2, %r6, 64;\n"
    "    add.s64 %rd3, %rd1, %rd2;\n"
    "    ld.global.u32 %r7, [%rd3];\n"
    "    add.u32 %r7, %r7, 1;\n"
    "    st.global.u32 [%rd3], %r7;\n"
    "    add.s64 %rd4, %rd3, 4;\n"
    "    st.global.u32 [%rd4], %r2;\n"
    "    add.s64 %rd5, %rd3, 12;\n"
    "    xor.b32 %r7, %r7, %r2;\n"
    "    st.global.u32 [%rd5], %r7;\n"
    "\n"
    "DONE:\n"
    "    ret;\n"
    "}\n";

static const BenchmarkMode modes[] = {
    { "host-bias",   CXL_BIAS_HOST,   0 },
    { "device-bias", CXL_BIAS_DEVICE, 0 },
    { "flip-per-iter", CXL_BIAS_HOST, 1 },
};

static uint64_t time_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int parse_int_arg(const char *arg, int fallback)
{
    char *end = NULL;
    long value;

    if (!arg || !*arg) {
        return fallback;
    }

    errno = 0;
    value = strtol(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0' || value <= 0 || value > 100000000L) {
        return fallback;
    }

    return (int)value;
}

static uint64_t parse_u64_arg(const char *arg, uint64_t fallback)
{
    char *end = NULL;
    unsigned long long value;

    if (!arg || !*arg) {
        return fallback;
    }

    errno = 0;
    value = strtoull(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0' || value == 0) {
        return fallback;
    }

    return (uint64_t)value;
}

static int encoded_bias(int bias_mode, uint64_t granularity)
{
    if (granularity == CXL_BIAS_GRAN_FLIT) {
        return bias_mode;
    }

    return (int)CXL_BIAS_ENCODE(bias_mode, granularity);
}

static int check_cuda(CUresult err, const char *call, int line)
{
    if (err != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA error %d at %s:%d (%s)\n", err, __FILE__, line, call);
        return -1;
    }
    return 0;
}

#define CHECK_CUDA(call) \
    do { \
        if (check_cuda((call), #call, __LINE__) != 0) { \
            return -1; \
        } \
    } while (0)

static void init_lines(SharedLine *lines, int line_count)
{
    int i;

    for (i = 0; i < line_count; i++) {
        lines[i].counter = 0;
        lines[i].gpu_seq = 0;
        lines[i].cpu_seq = 0;
        lines[i].checksum = 0;
        memset((void *)lines[i].pad, 0, sizeof(lines[i].pad));
    }
}

static int verify_lines(const SharedLine *lines, int line_count, uint32_t expected_seq, uint32_t expected_counter)
{
    int i;

    for (i = 0; i < line_count; i++) {
        if (lines[i].counter != expected_counter ||
            lines[i].gpu_seq != expected_seq ||
            lines[i].cpu_seq != expected_seq ||
            lines[i].checksum != (expected_counter ^ expected_seq)) {
            fprintf(stderr,
                    "Verification failed on line %d: counter=%u gpu_seq=%u cpu_seq=%u checksum=%u"
                    " expected=(%u,%u,%u)\n",
                    i,
                    lines[i].counter,
                    lines[i].gpu_seq,
                    lines[i].cpu_seq,
                    lines[i].checksum,
                    expected_counter,
                    expected_seq,
                    expected_counter ^ expected_seq);
            return -1;
        }
    }

    return 0;
}

static int run_kernel(CUfunction func, SharedLine *lines, int line_count, uint32_t seq)
{
    CUdeviceptr base = (CUdeviceptr)(uintptr_t)lines;
    unsigned int blocks = (unsigned int)((line_count + 63) / 64);
    unsigned int threads = 64;
    void *args[] = { &base, &line_count, &seq, NULL };

    CHECK_CUDA(cuLaunchKernel(func, blocks, 1, 1, threads, 1, 1, 0, NULL, args, NULL));
    CHECK_CUDA(cuCtxSynchronize());
    return 0;
}

static int run_mode(CUfunction func, SharedLine *lines, int line_count, int iterations,
                    int warmup, uint64_t bias_granularity, const BenchmarkMode *mode)
{
    uint64_t total_ns = 0;
    int iter;
    int measured = 0;
    const size_t region_size = (size_t)line_count * sizeof(*lines);
    CXLCoherencyStats stats;

    init_lines(lines, line_count);
    if (cxlSetBias(lines, region_size, encoded_bias(mode->initial_bias, bias_granularity)) != 0) {
        fprintf(stderr, "Failed to set initial bias for mode %s\n", mode->label);
        return -1;
    }
    cxlCoherentFence();

    if (cxlResetCoherencyStats() != 0) {
        fprintf(stderr, "Warning: could not reset coherency stats for mode %s\n", mode->label);
    }

    for (iter = 0; iter < warmup + iterations; iter++) {
        uint64_t start_ns;
        uint64_t end_ns;
        uint32_t seq = (uint32_t)(iter + 1);
        uint32_t expected_counter = (uint32_t)(2 * (iter + 1));
        int line;

        if (mode->flip_each_iteration) {
            if (cxlBiasFlip(lines, region_size, encoded_bias(CXL_BIAS_HOST, bias_granularity)) != 0) {
                fprintf(stderr, "Bias flip to host failed for mode %s\n", mode->label);
                return -1;
            }
        }

        start_ns = time_ns();
        for (line = 0; line < line_count; line++) {
            lines[line].cpu_seq = seq;
            lines[line].counter = lines[line].counter + 1;
        }
        cxlCoherentFence();

        if (mode->flip_each_iteration) {
            if (cxlBiasFlip(lines, region_size, encoded_bias(CXL_BIAS_DEVICE, bias_granularity)) != 0) {
                fprintf(stderr, "Bias flip to device failed for mode %s\n", mode->label);
                return -1;
            }
        }

        if (run_kernel(func, lines, line_count, seq) != 0) {
            return -1;
        }
        cxlCoherentFence();
        end_ns = time_ns();

        if (verify_lines(lines, line_count, seq, expected_counter) != 0) {
            return -1;
        }

        if (iter >= warmup) {
            total_ns += end_ns - start_ns;
            measured++;
        }
    }

    memset(&stats, 0, sizeof(stats));
    if (cxlGetCoherencyStats(&stats) != 0) {
        fprintf(stderr, "Warning: could not fetch coherency stats for mode %s\n", mode->label);
        memset(&stats, 0, sizeof(stats));
    }

    printf("\n[%s]\n", mode->label);
    printf("  total_ns=%" PRIu64 "\n", total_ns);
    printf("  avg_roundtrip_ns=%.1f\n", measured ? (double)total_ns / (double)measured : 0.0);
    printf("  avg_line_handoff_ns=%.1f\n",
           (measured && line_count > 0) ? (double)total_ns / ((double)measured * (double)line_count) : 0.0);
    printf("  handoffs_per_sec=%.2f\n",
           total_ns ? ((double)measured * (double)line_count * 1.0e9) / (double)total_ns : 0.0);
    printf("  coherency_requests=%" PRIu64 " back_invalidations=%" PRIu64
           " writebacks=%" PRIu64 " bias_flips=%" PRIu64 "\n",
           stats.coherency_requests,
           stats.back_invalidations,
           stats.writebacks,
           stats.bias_flips);
    printf("  snoop_hits=%" PRIu64 " snoop_misses=%" PRIu64
           " host_bias_hits=%" PRIu64 " device_bias_hits=%" PRIu64 "\n",
           stats.snoop_hits,
           stats.snoop_misses,
           stats.host_bias_hits,
           stats.device_bias_hits);
    printf("  upgrades=%" PRIu64 " downgrades=%" PRIu64 " directory_entries=%" PRIu64 "\n",
           stats.upgrades,
           stats.downgrades,
           stats.directory_entries);

    return 0;
}

int main(int argc, char **argv)
{
    int line_count = parse_int_arg(argc > 1 ? argv[1] : NULL, 64);
    int iterations = parse_int_arg(argc > 2 ? argv[2] : NULL, 1000);
    int warmup = parse_int_arg(argc > 3 ? argv[3] : NULL, 100);
    uint64_t bias_granularity = parse_u64_arg(argc > 4 ? argv[4] : NULL, CXL_BIAS_GRAN_FLIT);
    void *region = NULL;
    SharedLine *lines;
    CUdevice device;
    CUcontext ctx;
    CUmodule module;
    CUfunction func;
    size_t region_size;
    size_t i;

    printf("CPU/GPU HITM-style benchmark\n");
    printf("  lines=%d iterations=%d warmup=%d\n", line_count, iterations, warmup);
    printf("  bias_granularity=%" PRIu64 " bytes\n", bias_granularity);

    if (sizeof(SharedLine) != 64) {
        fprintf(stderr, "SharedLine must be exactly 64 bytes, got %zu\n", sizeof(SharedLine));
        return 1;
    }

    region_size = (size_t)line_count * sizeof(SharedLine);

    CHECK_CUDA(cuInit(0));
    CHECK_CUDA(cuDeviceGet(&device, 0));
    CHECK_CUDA(cuCtxCreate_v2(&ctx, 0, device));
    CHECK_CUDA(cuModuleLoadData(&module, hitm_ptx));
    CHECK_CUDA(cuModuleGetFunction(&func, module, "gpu_hitm_touch"));

    if (cxlCoherentAlloc((uint64_t)region_size, &region) != 0 || region == NULL) {
        fprintf(stderr, "cxlCoherentAlloc(%zu) failed\n", region_size);
        cuCtxDestroy_v2(ctx);
        return 1;
    }

    lines = (SharedLine *)region;
    printf("  coherent_region=%p size=%zu bytes\n", region, region_size);

    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (run_mode(func, lines, line_count, iterations, warmup, bias_granularity, &modes[i]) != 0) {
            cxlCoherentFree(region);
            cuCtxDestroy_v2(ctx);
            return 1;
        }
    }

    cxlCoherentFree(region);
    cuCtxDestroy_v2(ctx);
    return 0;
}
