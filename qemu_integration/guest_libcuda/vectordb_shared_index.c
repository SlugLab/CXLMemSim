#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <immintrin.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vectordb_shared_index_kernel_ptx.h"

#define CUDA_SUCCESS 0
#define CXL_COH_RANGE_READ 0
#define VECTORDB_TOPK 10U
#define VECTORDB_DIM 128U
#define VECTORDB_MAX_ROWS (1U << 18)
#define VECTORDB_MAX_QUERIES 64U
#define VECTORDB_CUDA_THREADS 256U
#define VECTORDB_ROWS_PER_BLOCK 8U
#define CACHE_LINE_BYTES 64U

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
typedef void *CUmodule;
typedef void *CUfunction;
typedef void *CUstream;
typedef uint64_t CUdeviceptr;

extern CUresult cuInit(unsigned int flags);
extern CUresult cuDeviceGetCount(int *count);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuDeviceGetName(char *name, int length, CUdevice device);
extern CUresult cuCtxCreate_v2(CUcontext *context, unsigned int flags, CUdevice device);
extern CUresult cuCtxDestroy_v2(CUcontext context);
extern CUresult cuCtxSynchronize(void);
extern CUresult cuModuleLoadData(CUmodule *module, const void *image);
extern CUresult cuModuleGetFunction(CUfunction *function, CUmodule module, const char *name);
extern CUresult cuModuleUnload(CUmodule module);
extern CUresult cuMemAlloc_v2(CUdeviceptr *device_pointer, size_t bytes);
extern CUresult cuMemFree_v2(CUdeviceptr device_pointer);
extern CUresult cuMemcpyHtoD_v2(CUdeviceptr destination, const void *source, size_t bytes);
extern CUresult cuMemcpyDtoH_v2(void *destination, CUdeviceptr source, size_t bytes);
extern CUresult cuLaunchKernel(CUfunction function, unsigned int grid_x, unsigned int grid_y, unsigned int grid_z,
                               unsigned int block_x, unsigned int block_y, unsigned int block_z,
                               unsigned int shared_bytes, CUstream stream, void **parameters, void **extra);

typedef int (*CxlCoherentAllocFn)(uint64_t size, void **host_pointer);
typedef int (*CxlCoherentFreeFn)(void *host_pointer);
typedef int (*CxlCoherentAcquireRangeFn)(void *host_pointer, uint64_t size, int intent, uint64_t *device_pointer,
                                         uint64_t *lines_granted);
typedef int (*CxlCoherentReleaseRangeFn)(void *host_pointer, uint64_t size, int dirty);

typedef enum {
    MODE_INVALID,
    MODE_TYPE2_HWCC,
    MODE_SOFTWARE_CC,
    MODE_FULL_COPY,
    MODE_NATIVE_GPU,
    MODE_NEGATIVE_STALE,
} BenchmarkMode;

typedef struct {
    BenchmarkMode mode;
    const char *mode_name;
    uint32_t rows;
    uint32_t dim;
    uint32_t queries;
    uint32_t topk;
    double update_ratio;
    uint32_t warmup;
    uint32_t epochs;
    uint64_t seed;
} Options;

typedef struct {
    CxlCoherentAllocFn alloc;
    CxlCoherentFreeFn free;
    CxlCoherentAcquireRangeFn acquire;
    CxlCoherentReleaseRangeFn release;
} CxlApi;

typedef struct {
    CUdevice device;
    CUcontext context;
    CUmodule module;
    CUfunction distance_kernel;
    CUfunction topk_kernel;
    char name[256];
} Gpu;

typedef struct {
    uint8_t *bits;
    size_t line_count;
    size_t dirty_count;
} DirtyLines;

typedef struct GrantEvidence {
    uint64_t lines_requested;
    uint64_t lines_granted;
    uint64_t partial_grants;
    const char *description;
} GrantEvidence;

static bool record_coherent_grant(GrantEvidence *grant, uint64_t device_lines_granted) {
    grant->lines_granted = device_lines_granted;
    if (device_lines_granted != grant->lines_requested) {
        ++grant->partial_grants;
        return false;
    }
    grant->description = "device-returned-exact-grant";
    return true;
}

static double now_ms(void) {
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0.0;
    return (double)value.tv_sec * 1000.0 + (double)value.tv_nsec / 1000000.0;
}

static bool checked_multiply_size(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left)
        return false;
    *result = left * right;
    return true;
}

static uint64_t next_random(uint64_t *state) {
    uint64_t value = *state;

    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * UINT64_C(2685821657736338717);
}

static float random_float(uint64_t *state) {
    return (float)(uint32_t)(next_random(state) >> 40) * (2.0f / 16777216.0f) - 1.0f;
}

__attribute__((noinline)) static void mmio_store_u32(volatile uint32_t *base, size_t index, uint32_t value) {
    base[index] = value;
}

static void mmio_store_float(float *base, size_t index, float value) {
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    mmio_store_u32((volatile uint32_t *)base, index, bits);
}

static BenchmarkMode parse_mode(const char *value) {
    if (strcmp(value, "type2-hwcc") == 0)
        return MODE_TYPE2_HWCC;
    if (strcmp(value, "software-cc") == 0)
        return MODE_SOFTWARE_CC;
    if (strcmp(value, "full-copy") == 0)
        return MODE_FULL_COPY;
    if (strcmp(value, "native-gpu") == 0)
        return MODE_NATIVE_GPU;
    if (strcmp(value, "negative-stale") == 0)
        return MODE_NEGATIVE_STALE;
    return MODE_INVALID;
}

static bool parse_u32(const char *text, uint32_t *value) {
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX)
        return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_u64(const char *text, uint64_t *value) {
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool parse_ratio(const char *text, double *value) {
    char *end = NULL;
    double parsed;

    errno = 0;
    parsed = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed) || parsed < 0.0 || parsed > 1.0)
        return false;
    *value = parsed;
    return true;
}

static void usage(const char *program) {
    fprintf(stderr,
            "usage: %s --mode type2-hwcc|software-cc|full-copy|native-gpu|negative-stale "
            "[--rows N] [--dim N] [--queries N] [--topk N] [--update-ratio R] "
            "[--warmup N] [--epochs N] [--seed N]\n",
            program);
}

static bool parse_options(int argc, char **argv, Options *options) {
    int index;

    *options = (Options){.mode = MODE_INVALID,
                         .rows = 16384,
                         .dim = VECTORDB_DIM,
                         .queries = 16,
                         .topk = VECTORDB_TOPK,
                         .update_ratio = 1.0 / 4096.0,
                         .warmup = 5,
                         .epochs = 10,
                         .seed = 1};
    for (index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        const char *value;

        if (index + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n", argument);
            return false;
        }
        value = argv[++index];
        if (strcmp(argument, "--mode") == 0) {
            options->mode = parse_mode(value);
            options->mode_name = value;
        } else if (strcmp(argument, "--rows") == 0) {
            if (!parse_u32(value, &options->rows))
                return false;
        } else if (strcmp(argument, "--dim") == 0) {
            if (!parse_u32(value, &options->dim))
                return false;
        } else if (strcmp(argument, "--queries") == 0) {
            if (!parse_u32(value, &options->queries))
                return false;
        } else if (strcmp(argument, "--topk") == 0) {
            if (!parse_u32(value, &options->topk))
                return false;
        } else if (strcmp(argument, "--update-ratio") == 0) {
            if (!parse_ratio(value, &options->update_ratio))
                return false;
        } else if (strcmp(argument, "--warmup") == 0) {
            if (!parse_u32(value, &options->warmup))
                return false;
        } else if (strcmp(argument, "--epochs") == 0) {
            if (!parse_u32(value, &options->epochs))
                return false;
        } else if (strcmp(argument, "--seed") == 0) {
            if (!parse_u64(value, &options->seed))
                return false;
        } else {
            fprintf(stderr, "unknown option: %s\n", argument);
            return false;
        }
    }
    return options->mode != MODE_INVALID && options->mode_name != NULL && options->rows >= options->topk &&
           options->rows <= VECTORDB_MAX_ROWS && options->dim == VECTORDB_DIM && options->queries != 0 &&
           options->queries <= VECTORDB_MAX_QUERIES && options->topk == VECTORDB_TOPK && options->epochs != 0 &&
           options->warmup <= UINT32_MAX - options->epochs;
}

static void *resolve_symbol(const char *name) {
    dlerror();
    return dlsym(RTLD_DEFAULT, name);
}

static CxlApi resolve_cxl_api(void) {
    CxlApi api = {0};
    void *symbol;

    symbol = resolve_symbol("cxlCoherentAlloc");
    memcpy(&api.alloc, &symbol, sizeof(api.alloc));
    symbol = resolve_symbol("cxlCoherentFree");
    memcpy(&api.free, &symbol, sizeof(api.free));
    symbol = resolve_symbol("cxlCoherentAcquireRange");
    memcpy(&api.acquire, &symbol, sizeof(api.acquire));
    symbol = resolve_symbol("cxlCoherentReleaseRange");
    memcpy(&api.release, &symbol, sizeof(api.release));
    return api;
}

static bool cuda_ok(CUresult result, const char *operation) {
    if (result == CUDA_SUCCESS)
        return true;
    fprintf(stderr, "CUDA operation failed: %s returned %d\n", operation, result);
    return false;
}

static bool contains_case_insensitive(const char *text, const char *needle) {
    size_t text_length = strlen(text);
    size_t needle_length = strlen(needle);

    if (needle_length > text_length)
        return false;
    for (size_t offset = 0; offset + needle_length <= text_length; ++offset) {
        size_t index;
        for (index = 0; index < needle_length; ++index) {
            if (tolower((unsigned char)text[offset + index]) != tolower((unsigned char)needle[index]))
                break;
        }
        if (index == needle_length)
            return true;
    }
    return false;
}

static void destroy_gpu(Gpu *gpu) {
    if (gpu->module != NULL)
        (void)cuModuleUnload(gpu->module);
    if (gpu->context != NULL)
        (void)cuCtxDestroy_v2(gpu->context);
    memset(gpu, 0, sizeof(*gpu));
}

static bool initialize_gpu(Gpu *gpu) {
    char *module_image = NULL;
    int count = 0;
    bool initialized = false;

    memset(gpu, 0, sizeof(*gpu));
    module_image = malloc((size_t)kVectordbSharedIndexPtx_len + 1);
    if (module_image == NULL)
        goto out;
    memcpy(module_image, kVectordbSharedIndexPtx, kVectordbSharedIndexPtx_len);
    module_image[kVectordbSharedIndexPtx_len] = '\0';
    if (!cuda_ok(cuInit(0), "cuInit") || !cuda_ok(cuDeviceGetCount(&count), "cuDeviceGetCount") || count <= 0 ||
        !cuda_ok(cuDeviceGet(&gpu->device, 0), "cuDeviceGet") ||
        !cuda_ok(cuDeviceGetName(gpu->name, (int)sizeof(gpu->name), gpu->device), "cuDeviceGetName") ||
        !cuda_ok(cuCtxCreate_v2(&gpu->context, 0, gpu->device), "cuCtxCreate_v2") ||
        !cuda_ok(cuModuleLoadData(&gpu->module, module_image), "cuModuleLoadData") ||
        !cuda_ok(cuModuleGetFunction(&gpu->distance_kernel, gpu->module, "vectordb_distance_tiled"),
                 "cuModuleGetFunction(vectordb_distance_tiled)") ||
        !cuda_ok(cuModuleGetFunction(&gpu->topk_kernel, gpu->module, "vectordb_topk_tiled"),
                 "cuModuleGetFunction(vectordb_topk_tiled)"))
        goto out;
    if (contains_case_insensitive(gpu->name, "mock") || contains_case_insensitive(gpu->name, "simulat") ||
        contains_case_insensitive(gpu->name, "emulat")) {
        fprintf(stderr, "refusing non-physical GPU identity: %s\n", gpu->name);
        goto out;
    }
    initialized = true;

out:
    free(module_image);
    if (!initialized)
        destroy_gpu(gpu);
    return initialized;
}

static bool initialize_dirty_lines(DirtyLines *dirty, size_t bytes) {
    dirty->line_count = (bytes + CACHE_LINE_BYTES - 1) / CACHE_LINE_BYTES;
    dirty->bits = calloc((dirty->line_count + 7) / 8, 1);
    dirty->dirty_count = 0;
    return dirty->bits != NULL;
}

static void clear_dirty_lines(DirtyLines *dirty) {
    memset(dirty->bits, 0, (dirty->line_count + 7) / 8);
    dirty->dirty_count = 0;
}

static bool line_is_dirty(const DirtyLines *dirty, size_t line) {
    return (dirty->bits[line / 8] & (uint8_t)(1U << (line % 8))) != 0;
}

static void mark_dirty_range(DirtyLines *dirty, size_t offset, size_t bytes) {
    size_t first = offset / CACHE_LINE_BYTES;
    size_t last = (offset + bytes - 1) / CACHE_LINE_BYTES;

    for (size_t line = first; line <= last; ++line) {
        uint8_t mask = (uint8_t)(1U << (line % 8));
        if ((dirty->bits[line / 8] & mask) == 0) {
            dirty->bits[line / 8] |= mask;
            ++dirty->dirty_count;
        }
    }
}

__attribute__((target("clflushopt"))) static void flush_cache_line(void *address) { _mm_clflushopt(address); }

static bool publish_software_cc(float *source, const float *copy_source, CUdeviceptr replica, const DirtyLines *dirty,
                                uint64_t *copied_bytes) {
    size_t line = 0;

    if (!__builtin_cpu_supports("clflushopt")) {
        fprintf(stderr, "software-cc requires CLFLUSHOPT\n");
        return false;
    }
    for (line = 0; line < dirty->line_count; ++line) {
        if (line_is_dirty(dirty, line))
            flush_cache_line((uint8_t *)source + line * CACHE_LINE_BYTES);
    }
    _mm_sfence();
    line = 0;
    while (line < dirty->line_count) {
        size_t first;
        size_t bytes;

        while (line < dirty->line_count && !line_is_dirty(dirty, line))
            ++line;
        first = line;
        while (line < dirty->line_count && line_is_dirty(dirty, line))
            ++line;
        if (first == line)
            continue;
        bytes = (line - first) * CACHE_LINE_BYTES;
        if (!cuda_ok(cuMemcpyHtoD_v2(replica + first * CACHE_LINE_BYTES,
                                     (const uint8_t *)copy_source + first * CACHE_LINE_BYTES, bytes),
                     "cuMemcpyHtoD_v2(dirty range)"))
            return false;
        *copied_bytes += bytes;
    }
    return cuda_ok(cuCtxSynchronize(), "cuCtxSynchronize(software-cc)");
}

static size_t update_count_for(const Options *options) {
    double exact = (double)options->rows * options->update_ratio;
    size_t count = (size_t)exact;

    if ((double)count < exact)
        ++count;
    if ((options->update_ratio > 0.0 || options->mode == MODE_NEGATIVE_STALE) && count == 0)
        count = 1;
    return count > options->rows ? options->rows : count;
}

static void update_source(float *source, float *oracle_source, bool source_is_cxl, const Options *options,
                          uint32_t iteration, DirtyLines *dirty, uint32_t *target_query, uint32_t *target_label) {
    size_t row_bytes = (size_t)options->dim * sizeof(float);
    size_t count = update_count_for(options);

    clear_dirty_lines(dirty);
    *target_query = iteration % options->queries;
    *target_label = *target_query % options->rows;
    for (size_t updated = 0; updated < count; ++updated) {
        uint32_t row = updated == 0 ? *target_label : (*target_label + (uint32_t)(updated * 7919U)) % options->rows;
        for (uint32_t dimension = 0; dimension < options->dim; ++dimension) {
            size_t index = (size_t)row * options->dim + dimension;
            float value = 8.0f + (float)iteration * 0.01f + (float)row * 0.000001f + (float)dimension * 0.0001f;

            oracle_source[index] = value;
            if (source_is_cxl)
                mmio_store_float(source, index, value);
        }
        mark_dirty_range(dirty, (size_t)row * row_bytes, row_bytes);
    }
}

static void insert_topk(float distance, int label, float *distances, int *labels, uint32_t topk) {
    for (uint32_t position = 0; position < topk; ++position) {
        if (distance < distances[position] || (distance == distances[position] && label < labels[position])) {
            for (uint32_t move = topk - 1; move > position; --move) {
                distances[move] = distances[move - 1];
                labels[move] = labels[move - 1];
            }
            distances[position] = distance;
            labels[position] = label;
            return;
        }
    }
}

static void cpu_exact_oracle(const float *source, const float *queries, const Options *options, int *labels,
                             float *distances) {
    for (uint32_t query = 0; query < options->queries; ++query) {
        float *query_distances = distances + (size_t)query * options->topk;
        int *query_labels = labels + (size_t)query * options->topk;

        for (uint32_t item = 0; item < options->topk; ++item) {
            query_distances[item] = INFINITY;
            query_labels[item] = INT_MAX;
        }
        for (uint32_t row = 0; row < options->rows; ++row) {
            float distance = 0.0f;
            for (uint32_t dimension = 0; dimension < options->dim; ++dimension) {
                float delta =
                    source[(size_t)row * options->dim + dimension] - queries[(size_t)query * options->dim + dimension];
                distance += delta * delta;
            }
            insert_topk(distance, (int)row, query_distances, query_labels, options->topk);
        }
    }
}

static bool initialize_query_timing(CUdeviceptr device_query_start_ns, uint64_t *query_elapsed_ns, size_t timing_bytes,
                                    uint32_t queries) {
    for (uint32_t query = 0; query < queries; ++query)
        query_elapsed_ns[query] = UINT64_MAX;
    return cuda_ok(cuMemcpyHtoD_v2(device_query_start_ns, query_elapsed_ns, timing_bytes),
                   "cuMemcpyHtoD_v2(query_start_ns)");
}

static bool launch_search(const Gpu *gpu, CUdeviceptr index, CUdeviceptr queries, const Options *options,
                          CUdeviceptr distance_scratch, CUdeviceptr labels, CUdeviceptr distances,
                          CUdeviceptr query_start_ns, CUdeviceptr query_elapsed_ns) {
    uint32_t rows = options->rows;
    uint32_t dim = options->dim;
    uint32_t topk = options->topk;
    uint32_t distance_grid_x = (options->rows + VECTORDB_ROWS_PER_BLOCK - 1) / VECTORDB_ROWS_PER_BLOCK;
    void *distance_parameters[] = {&index, &queries, &rows, &dim, &distance_scratch, &query_start_ns, NULL};
    void *topk_parameters[] = {&distance_scratch, &rows, &topk, &labels, &distances, &query_start_ns,
                               &query_elapsed_ns, NULL};

    return cuda_ok(cuLaunchKernel(gpu->distance_kernel, distance_grid_x, options->queries, 1, 256, 1, 1, 0, NULL,
                                  distance_parameters, NULL),
                   "cuLaunchKernel(vectordb_distance_tiled)") &&
           cuda_ok(cuLaunchKernel(gpu->topk_kernel, options->queries, 1, 1, 256, 1, 1, 0, NULL, topk_parameters, NULL),
                   "cuLaunchKernel(vectordb_topk_tiled)") &&
           cuda_ok(cuCtxSynchronize(), "cuCtxSynchronize(kernels)");
}

static int compare_u64(const void *left, const void *right) {
    uint64_t left_value = *(const uint64_t *)left;
    uint64_t right_value = *(const uint64_t *)right;
    return (left_value > right_value) - (left_value < right_value);
}

static uint64_t percentile_nearest_rank_ns(const uint64_t *sorted_samples, size_t count, unsigned percentile) {
    size_t rank;

    if (count == 0 || percentile == 0 || percentile > 100)
        return 0;
    rank = ((size_t)percentile * count + 99U) / 100U;
    return sorted_samples[rank - 1];
}

static bool compute_query_percentiles_ms(uint64_t *query_elapsed_ns, size_t query_count, double *p50_ms,
                                         double *p99_ms) {
    for (size_t query = 0; query < query_count; ++query) {
        if (query_elapsed_ns[query] == 0 || query_elapsed_ns[query] == UINT64_MAX) {
            fprintf(stderr, "invalid GPU query timing at query %zu: %" PRIu64 "\n", query, query_elapsed_ns[query]);
            return false;
        }
    }
    qsort(query_elapsed_ns, query_count, sizeof(*query_elapsed_ns), compare_u64);
    *p50_ms = (double)percentile_nearest_rank_ns(query_elapsed_ns, query_count, 50) / 1000000.0;
    *p99_ms = (double)percentile_nearest_rank_ns(query_elapsed_ns, query_count, 99) / 1000000.0;
    return true;
}

static bool results_match(const Options *options, const int *gpu_labels, const float *gpu_distances,
                          const int *cpu_labels, const float *cpu_distances, float *maximum_error) {
    size_t count = (size_t)options->queries * options->topk;

    *maximum_error = 0.0f;
    for (size_t index = 0; index < count; ++index) {
        float error = fabsf(gpu_distances[index] - cpu_distances[index]);
        float tolerance = 1.0e-4f * fmaxf(1.0f, fabsf(cpu_distances[index]));
        if (error > *maximum_error)
            *maximum_error = error;
        if (gpu_labels[index] != cpu_labels[index] || !isfinite(gpu_distances[index]) || error > tolerance)
            return false;
    }
    return true;
}

static void print_json_string(const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;

    putchar('"');
    while (*cursor != '\0') {
        if (*cursor == '"' || *cursor == '\\') {
            putchar('\\');
            putchar(*cursor);
        } else if (*cursor < 0x20) {
            printf("\\u%04x", *cursor);
        } else {
            putchar(*cursor);
        }
        ++cursor;
    }
    putchar('"');
}

static void print_label_array(const int *labels, size_t count) {
    putchar('[');
    for (size_t index = 0; index < count; ++index) {
        if (index != 0)
            putchar(',');
        printf("%d", labels[index]);
    }
    putchar(']');
}

static void emit_result(const Options *options, const Gpu *gpu, const char *backend, uint32_t epoch,
                        double end_to_end_ms, double update_ms, double synchronization_ms, double kernel_ms,
                        double oracle_ms, double p50_query_ms, double p99_query_ms, uint64_t copied_bytes,
                        size_t dirty_lines, const GrantEvidence *grant, bool correct, bool stale_observed,
                        uint32_t target_query, uint32_t target_label, const int *gpu_labels, const int *cpu_labels,
                        float maximum_error) {
    size_t result_count = (size_t)options->queries * options->topk;
    double qps = end_to_end_ms > 0.0 ? (double)options->queries * 1000.0 / end_to_end_ms : 0.0;
    int observed_label = gpu_labels[(size_t)target_query * options->topk];
    int expected_label = cpu_labels[(size_t)target_query * options->topk];

    printf("{\"schema\":\"splash.vectordb.v1\",\"mode\":");
    print_json_string(options->mode_name);
    printf(",\"backend\":");
    print_json_string(backend);
    printf(",\"gpu_name\":");
    print_json_string(gpu->name);
    printf(",\"gpu_ordinal\":0,\"real_gpu\":true,\"rows\":%u,\"dim\":%u,\"queries\":%u,"
           "\"topk\":%u,\"update_ratio\":%.12g,\"warmup\":%u,\"epochs\":%u,\"seed\":%" PRIu64
           ",\"epoch\":%u,\"end_to_end_ms\":%.6f,\"update_ms\":%.6f,\"synchronization_ms\":%.6f,"
           "\"kernel_ms\":%.6f,\"oracle_ms\":%.6f,\"qps\":%.6f,\"p50_query_ms\":%.6f,"
           "\"p99_query_ms\":%.6f,\"copied_bytes\":%" PRIu64 ",\"dirty_lines\":%zu,"
           "\"lines_requested\":%" PRIu64 ",\"lines_granted\":%" PRIu64 ",\"partial_grants\":%" PRIu64
           ",\"grant_evidence\":",
           options->rows, options->dim, options->queries, options->topk, options->update_ratio, options->warmup,
           options->epochs, options->seed, epoch, end_to_end_ms, update_ms, synchronization_ms, kernel_ms, oracle_ms,
           qps, p50_query_ms, p99_query_ms, copied_bytes, dirty_lines, grant->lines_requested, grant->lines_granted,
           grant->partial_grants);
    print_json_string(grant->description);
    printf(",\"correct\":%s,\"stale_observed\":%s,\"target_query\":%u,\"old_label\":%u,"
           "\"observed_label\":%d,\"expected_label\":%d,\"max_distance_error\":%.9g,\"gpu_labels\":",
           correct ? "true" : "false", stale_observed ? "true" : "false", target_query, target_label, observed_label,
           expected_label, maximum_error);
    print_label_array(gpu_labels, result_count);
    printf(",\"cpu_labels\":");
    print_label_array(cpu_labels, result_count);
    printf("}\n");
    fflush(stdout);
}

static bool free_device_allocation(CUdeviceptr *pointer, const char *name) {
    bool result;

    if (*pointer == 0)
        return true;
    result = cuda_ok(cuMemFree_v2(*pointer), name);
    *pointer = 0;
    return result;
}

int main(int argc, char **argv) {
    Options options;
    CxlApi cxl;
    Gpu gpu;
    float *source = NULL;
    float *oracle_source = NULL;
    float *host_queries = NULL;
    int *gpu_labels = NULL;
    int *cpu_labels = NULL;
    float *gpu_distances = NULL;
    float *cpu_distances = NULL;
    uint64_t *query_elapsed_ns = NULL;
    DirtyLines dirty = {0};
    CUdeviceptr device_index = 0;
    CUdeviceptr device_queries = 0;
    CUdeviceptr device_labels = 0;
    CUdeviceptr device_distances = 0;
    CUdeviceptr device_distance_scratch = 0;
    CUdeviceptr device_query_start_ns = 0;
    CUdeviceptr device_query_elapsed_ns = 0;
    size_t index_elements, index_bytes, query_elements, query_bytes, result_count;
    size_t label_bytes, distance_bytes, scratch_elements, scratch_bytes, timing_bytes;
    uint64_t random_state;
    GrantEvidence grant_evidence = {0, 0, 0, "not-applicable"};
    bool source_is_cxl = false;
    bool gpu_initialized = false;
    bool type2_range_active = false;
    int exit_code = 1;
    const char *backend;

    if (!parse_options(argc, argv, &options)) {
        usage(argv[0]);
        return 2;
    }
    if (!checked_multiply_size(options.rows, options.dim, &index_elements) ||
        !checked_multiply_size(index_elements, sizeof(float), &index_bytes) ||
        !checked_multiply_size(options.queries, options.dim, &query_elements) ||
        !checked_multiply_size(query_elements, sizeof(float), &query_bytes) ||
        !checked_multiply_size(options.queries, options.topk, &result_count) ||
        !checked_multiply_size(result_count, sizeof(int), &label_bytes) ||
        !checked_multiply_size(result_count, sizeof(float), &distance_bytes) ||
        !checked_multiply_size(options.rows, options.queries, &scratch_elements) ||
        !checked_multiply_size(scratch_elements, sizeof(float), &scratch_bytes) ||
        !checked_multiply_size(options.queries, sizeof(uint64_t), &timing_bytes) ||
        index_bytes % CACHE_LINE_BYTES != 0) {
        fprintf(stderr, "configuration size overflow or non-cache-line matrix size\n");
        return 2;
    }

    cxl = resolve_cxl_api();
#ifdef VECTORDB_NATIVE_BUILD
    if (cxl.alloc != NULL || cxl.free != NULL || cxl.acquire != NULL || cxl.release != NULL) {
        fprintf(stderr, "native build resolved CXL shim symbols; refusing mixed backend\n");
        return 3;
    }
#endif
    if (!initialize_gpu(&gpu))
        goto cleanup;
    gpu_initialized = true;
    if (!contains_case_insensitive(gpu.name, "NVIDIA")) {
        fprintf(stderr, "real NVIDIA GPU identity required, got: %s\n", gpu.name);
        goto cleanup;
    }

    source_is_cxl =
        options.mode == MODE_TYPE2_HWCC || options.mode == MODE_SOFTWARE_CC || options.mode == MODE_FULL_COPY;
    if (source_is_cxl) {
        void *coherent_source = NULL;
        if (cxl.alloc == NULL || cxl.free == NULL ||
            (options.mode == MODE_TYPE2_HWCC && (cxl.acquire == NULL || cxl.release == NULL))) {
            fprintf(stderr, "mode %s requires its coherent allocation and range APIs\n", options.mode_name);
            goto cleanup;
        }
        if (cxl.alloc(index_bytes, &coherent_source) != CUDA_SUCCESS || coherent_source == NULL) {
            fprintf(stderr, "cxlCoherentAlloc failed for %zu bytes\n", index_bytes);
            goto cleanup;
        }
        source = coherent_source;
    } else {
        void *aligned_source = NULL;
        if (posix_memalign(&aligned_source, CACHE_LINE_BYTES, index_bytes) != 0) {
            fprintf(stderr, "host index allocation failed\n");
            goto cleanup;
        }
        source = aligned_source;
    }
    if (((uintptr_t)source % CACHE_LINE_BYTES) != 0) {
        fprintf(stderr, "source matrix is not cache-line aligned\n");
        goto cleanup;
    }
    if (source_is_cxl) {
        void *aligned_oracle_source = NULL;
        if (posix_memalign(&aligned_oracle_source, CACHE_LINE_BYTES, index_bytes) != 0) {
            fprintf(stderr, "oracle index allocation failed\n");
            goto cleanup;
        }
        oracle_source = aligned_oracle_source;
    } else {
        oracle_source = source;
    }
    {
        void *aligned_queries = NULL;
        if (posix_memalign(&aligned_queries, CACHE_LINE_BYTES, query_bytes) != 0) {
            fprintf(stderr, "host query allocation failed\n");
            goto cleanup;
        }
        host_queries = aligned_queries;
    }
    if (!initialize_dirty_lines(&dirty, index_bytes)) {
        fprintf(stderr, "dirty-line allocation failed\n");
        goto cleanup;
    }
    gpu_labels = malloc(label_bytes);
    cpu_labels = malloc(label_bytes);
    gpu_distances = malloc(distance_bytes);
    cpu_distances = malloc(distance_bytes);
    query_elapsed_ns = malloc(timing_bytes);
    if (gpu_labels == NULL || cpu_labels == NULL || gpu_distances == NULL || cpu_distances == NULL ||
        query_elapsed_ns == NULL) {
        fprintf(stderr, "result allocation failed\n");
        goto cleanup;
    }

    random_state = options.seed != 0 ? options.seed : UINT64_C(0x9e3779b97f4a7c15);
    for (size_t index = 0; index < index_elements; ++index) {
        float value = random_float(&random_state);

        oracle_source[index] = value;
        if (source_is_cxl)
            mmio_store_float(source, index, value);
    }
    for (uint32_t query = 0; query < options.queries; ++query)
        memcpy(host_queries + (size_t)query * options.dim, oracle_source + (size_t)(query % options.rows) * options.dim,
               (size_t)options.dim * sizeof(float));

    if (options.mode != MODE_TYPE2_HWCC &&
        (!cuda_ok(cuMemAlloc_v2(&device_index, index_bytes), "cuMemAlloc_v2(index)") ||
         !cuda_ok(cuMemcpyHtoD_v2(device_index, oracle_source, index_bytes), "cuMemcpyHtoD_v2(initial index)")))
        goto cleanup;
    if (!cuda_ok(cuMemAlloc_v2(&device_queries, query_bytes), "cuMemAlloc_v2(queries)") ||
        !cuda_ok(cuMemAlloc_v2(&device_labels, label_bytes), "cuMemAlloc_v2(labels)") ||
        !cuda_ok(cuMemAlloc_v2(&device_distances, distance_bytes), "cuMemAlloc_v2(distances)") ||
        !cuda_ok(cuMemAlloc_v2(&device_distance_scratch, scratch_bytes), "cuMemAlloc_v2(distance scratch)") ||
        !cuda_ok(cuMemAlloc_v2(&device_query_start_ns, timing_bytes), "cuMemAlloc_v2(query start timing)") ||
        !cuda_ok(cuMemAlloc_v2(&device_query_elapsed_ns, timing_bytes), "cuMemAlloc_v2(query elapsed timing)") ||
        !cuda_ok(cuMemcpyHtoD_v2(device_queries, host_queries, query_bytes), "cuMemcpyHtoD_v2(queries)") ||
        !cuda_ok(cuCtxSynchronize(), "cuCtxSynchronize(setup)"))
        goto cleanup;

    if (options.mode == MODE_TYPE2_HWCC) {
        uint64_t acquired_device_pointer = 0;
        uint64_t initial_lines_granted = 0;
        CUresult acquire_result;
        bool exact_grant;

        grant_evidence.lines_requested = index_bytes / CACHE_LINE_BYTES;
        acquire_result =
            cxl.acquire(source, index_bytes, CXL_COH_RANGE_READ, &acquired_device_pointer, &initial_lines_granted);
        exact_grant = record_coherent_grant(&grant_evidence, initial_lines_granted);
        if (acquire_result != CUDA_SUCCESS || acquired_device_pointer == 0 || !exact_grant) {
            fprintf(stderr,
                    "initial cxlCoherentAcquireRange failed or returned a partial grant (%" PRIu64 "/%" PRIu64
                    ", partial_grants=%" PRIu64 ")\n",
                    grant_evidence.lines_granted, grant_evidence.lines_requested, grant_evidence.partial_grants);
            goto cleanup;
        }
        device_index = acquired_device_pointer;
        type2_range_active = true;
    }

    backend = source_is_cxl ? "qemu-type2-hetgpu" : "cuda-driver";
    for (uint32_t iteration = 0; iteration < options.warmup + options.epochs; ++iteration) {
        uint32_t target_query = iteration % options.queries;
        uint32_t target_label = target_query % options.rows;
        uint64_t copied_bytes = 0;
        uint64_t acquired_device_pointer = 0;
        float maximum_error = 0.0f;
        bool correct;
        bool stale_observed;
        double epoch_start = now_ms();
        double update_start = epoch_start;
        double update_end;
        double synchronization_start;
        double synchronization_ms;
        double kernel_start;
        double kernel_end;
        double epoch_end;
        double oracle_start;
        double oracle_end;
        double p50_query_ms;
        double p99_query_ms;

        if (options.mode != MODE_NATIVE_GPU)
            update_source(source, oracle_source, source_is_cxl, &options, iteration, &dirty, &target_query,
                          &target_label);
        else
            clear_dirty_lines(&dirty);
        update_end = now_ms();

        synchronization_start = now_ms();
        if (options.mode == MODE_TYPE2_HWCC && dirty.dirty_count != 0) {
            uint64_t post_update_lines_granted = 0;
            CUresult acquire_result = cxl.acquire(source, index_bytes, CXL_COH_RANGE_READ, &acquired_device_pointer,
                                                  &post_update_lines_granted);
            bool exact_grant = record_coherent_grant(&grant_evidence, post_update_lines_granted);

            if (acquire_result != CUDA_SUCCESS || acquired_device_pointer != device_index || !exact_grant) {
                fprintf(stderr,
                        "post-update cxlCoherentAcquireRange failed, remapped, or returned a partial grant "
                        "(%" PRIu64 "/%" PRIu64 ", partial_grants=%" PRIu64 ")\n",
                        grant_evidence.lines_granted, grant_evidence.lines_requested, grant_evidence.partial_grants);
                goto cleanup;
            }
        } else if (options.mode == MODE_SOFTWARE_CC) {
            if (!publish_software_cc(source, oracle_source, device_index, &dirty, &copied_bytes))
                goto cleanup;
        } else if (options.mode == MODE_FULL_COPY) {
            if (!cuda_ok(cuMemcpyHtoD_v2(device_index, oracle_source, index_bytes), "cuMemcpyHtoD_v2(full index)") ||
                !cuda_ok(cuCtxSynchronize(), "cuCtxSynchronize(full-copy)"))
                goto cleanup;
            copied_bytes = index_bytes;
        }
        synchronization_ms = now_ms() - synchronization_start;
        if (!initialize_query_timing(device_query_start_ns, query_elapsed_ns, timing_bytes, options.queries))
            goto cleanup;

        kernel_start = now_ms();
        if (!launch_search(&gpu, device_index, device_queries, &options, device_distance_scratch, device_labels,
                           device_distances, device_query_start_ns, device_query_elapsed_ns))
            goto cleanup;
        kernel_end = now_ms();
        if (!cuda_ok(cuMemcpyDtoH_v2(gpu_labels, device_labels, label_bytes), "cuMemcpyDtoH_v2(labels)") ||
            !cuda_ok(cuMemcpyDtoH_v2(gpu_distances, device_distances, distance_bytes), "cuMemcpyDtoH_v2(distances)") ||
            !cuda_ok(cuMemcpyDtoH_v2(query_elapsed_ns, device_query_elapsed_ns, timing_bytes),
                     "cuMemcpyDtoH_v2(query_elapsed_ns)") ||
            !compute_query_percentiles_ms(query_elapsed_ns, options.queries, &p50_query_ms, &p99_query_ms))
            goto cleanup;
        epoch_end = now_ms();

        oracle_start = now_ms();
        cpu_exact_oracle(oracle_source, host_queries, &options, cpu_labels, cpu_distances);
        oracle_end = now_ms();
        correct = results_match(&options, gpu_labels, gpu_distances, cpu_labels, cpu_distances, &maximum_error);
        stale_observed = options.mode == MODE_NEGATIVE_STALE &&
                         gpu_labels[(size_t)target_query * options.topk] == (int)target_label &&
                         cpu_labels[(size_t)target_query * options.topk] != (int)target_label;
        if ((options.mode == MODE_NEGATIVE_STALE && !stale_observed) ||
            (options.mode != MODE_NEGATIVE_STALE && !correct) ||
            (options.mode == MODE_TYPE2_HWCC && copied_bytes != 0)) {
            fprintf(stderr,
                    "correctness gate failed: mode=%s correct=%s stale_observed=%s target=%u gpu=%d cpu=%d "
                    "max_error=%.9g\n",
                    options.mode_name, correct ? "true" : "false", stale_observed ? "true" : "false", target_label,
                    gpu_labels[(size_t)target_query * options.topk], cpu_labels[(size_t)target_query * options.topk],
                    maximum_error);
            goto cleanup;
        }
        if (type2_range_active && iteration + 1 == options.warmup + options.epochs) {
            if (cxl.release(source, index_bytes, 0) != CUDA_SUCCESS) {
                fprintf(stderr, "cxlCoherentReleaseRange failed\n");
                goto cleanup;
            }
            type2_range_active = false;
        }
        if (iteration >= options.warmup) {
            emit_result(&options, &gpu, backend, iteration - options.warmup, epoch_end - epoch_start,
                        update_end - update_start, synchronization_ms, kernel_end - kernel_start,
                        oracle_end - oracle_start, p50_query_ms, p99_query_ms, copied_bytes, dirty.dirty_count,
                        &grant_evidence, correct, stale_observed, target_query, target_label, gpu_labels, cpu_labels,
                        maximum_error);
        }
    }
    exit_code = 0;

cleanup:
    if (type2_range_active && cxl.release != NULL) {
        if (cxl.release(source, index_bytes, 0) != CUDA_SUCCESS)
            exit_code = 1;
        type2_range_active = false;
    }
    if (!free_device_allocation(&device_query_elapsed_ns, "cuMemFree_v2(query elapsed timing)"))
        exit_code = 1;
    if (!free_device_allocation(&device_query_start_ns, "cuMemFree_v2(query start timing)"))
        exit_code = 1;
    if (!free_device_allocation(&device_distance_scratch, "cuMemFree_v2(distance scratch)"))
        exit_code = 1;
    if (!free_device_allocation(&device_distances, "cuMemFree_v2(distances)"))
        exit_code = 1;
    if (!free_device_allocation(&device_labels, "cuMemFree_v2(labels)"))
        exit_code = 1;
    if (!free_device_allocation(&device_queries, "cuMemFree_v2(queries)"))
        exit_code = 1;
    if (options.mode != MODE_TYPE2_HWCC && !free_device_allocation(&device_index, "cuMemFree_v2(index)"))
        exit_code = 1;
    free(dirty.bits);
    free(query_elapsed_ns);
    free(cpu_distances);
    free(gpu_distances);
    free(cpu_labels);
    free(gpu_labels);
    free(host_queries);
    if (source_is_cxl)
        free(oracle_source);
    if (source != NULL) {
        if (source_is_cxl && cxl.free != NULL) {
            if (cxl.free(source) != CUDA_SUCCESS)
                exit_code = 1;
        } else {
            free(source);
        }
    }
    if (gpu_initialized)
        destroy_gpu(&gpu);
    return exit_code;
}
