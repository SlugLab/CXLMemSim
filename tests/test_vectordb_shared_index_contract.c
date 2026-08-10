#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VECTORDB_SOURCE_PATH
#error "VECTORDB_SOURCE_PATH must identify the benchmark source"
#endif

#ifndef VECTORDB_KERNEL_SOURCE_PATH
#error "VECTORDB_KERNEL_SOURCE_PATH must identify the CUDA kernel source"
#endif

static char *read_source(const char *path) {
    FILE *file = fopen(path, "rb");
    long size;
    char *source;

    if (file == NULL) {
        fprintf(stderr, "benchmark source is absent: %s\n", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    source = malloc((size_t)size + 1);
    if (source == NULL || fread(source, 1, (size_t)size, file) != (size_t)size) {
        free(source);
        fclose(file);
        return NULL;
    }
    source[size] = '\0';
    fclose(file);
    return source;
}

static bool ordered(const char *source, const char *first, const char *second) {
    const char *first_match = strstr(source, first);
    const char *second_match = strstr(source, second);

    if (first_match == NULL || second_match == NULL || first_match >= second_match) {
        fprintf(stderr, "benchmark contract ordering failed: %s must precede %s\n", first, second);
        return false;
    }
    return true;
}

static bool contains(const char *source, const char *needle) {
    if (strstr(source, needle) == NULL) {
        fprintf(stderr, "missing benchmark contract token: %s\n", needle);
        return false;
    }
    return true;
}

static bool rejects(const char *source, const char *needle) {
    if (strstr(source, needle) != NULL) {
        fprintf(stderr, "forbidden benchmark implementation: %s\n", needle);
        return false;
    }
    return true;
}

static bool contains_at_least(const char *source, const char *needle, size_t minimum) {
    size_t count = 0;
    size_t needle_length = strlen(needle);
    const char *match = source;

    while ((match = strstr(match, needle)) != NULL) {
        ++count;
        match += needle_length;
    }
    if (count < minimum) {
        fprintf(stderr, "benchmark contract token %s occurs %zu times, expected at least %zu\n", needle, count,
                minimum);
        return false;
    }
    return true;
}

int main(void) {
    static const char *const required[] = {
        "schema\\\":\\\"splash.vectordb.v1",
        "type2-hwcc",
        "software-cc",
        "full-copy",
        "native-gpu",
        "negative-stale",
        "--rows",
        "--dim",
        "--queries",
        "--topk",
        "--update-ratio",
        "--warmup",
        "--epochs",
        "--seed",
        "dlsym",
        "cxlCoherentAlloc",
        "cxlCoherentFree",
        "cxlCoherentAcquireRange",
        "cxlCoherentReleaseRange",
        "cuLaunchKernel",
        "vectordb_distance_tiled",
        "vectordb_topk_tiled",
        "VECTORDB_CUDA_THREADS",
        "VECTORDB_ROWS_PER_BLOCK",
        "distance_scratch",
        "device_query_start_ns",
        "UINT64_MAX",
        "options->queries, 1, 256, 1, 1",
        "distance_grid_x, options->queries, 1, 256, 1, 1",
        "gpu->topk_kernel, options->queries, 1, 1, 256, 1, 1",
        "percentile_nearest_rank_ns",
        "CUdeviceptr device_query_elapsed_ns",
        "cuMemcpyDtoH_v2(query_elapsed_ns",
        "_mm_clflushopt",
        "_mm_sfence",
        "copied_bytes",
        "dirty_lines",
        "typedef struct GrantEvidence",
        "uint64_t lines_requested;",
        "uint64_t lines_granted;",
        "uint64_t partial_grants;",
        "GrantEvidence grant_evidence = {0, 0, 0, \"not-applicable\"};",
        "grant_evidence.lines_requested = index_bytes / CACHE_LINE_BYTES;",
        "record_coherent_grant(&grant_evidence, initial_lines_granted)",
        "record_coherent_grant(&grant_evidence, post_update_lines_granted)",
        "grant->lines_granted = device_lines_granted;",
        "++grant->partial_grants;",
        "device-returned-exact-grant",
        "not-applicable",
        "\\\"lines_requested\\\":%",
        "\\\"lines_granted\\\":%",
        "\\\"partial_grants\\\":%",
        "\\\"grant_evidence\\\":",
        "correct",
        "stale_observed",
        "gpu_labels",
        "cpu_labels",
        "options.mode == MODE_TYPE2_HWCC && copied_bytes != 0",
    };
    static const char *const kernel_required[] = {
        "__global__ void vectordb_distance_tiled",
        "__global__ void vectordb_topk_tiled",
        "__shared__ float candidate_distances[256 * 10]",
        "__shared__ int candidate_labels[256 * 10]",
        "threadIdx.x / 32",
        "threadIdx.x % 32",
        "dimension += 32",
        "row += blockDim.x",
        "atomicMin(query_start_ns + query",
        "%globaltimer",
    };
    char *source = read_source(VECTORDB_SOURCE_PATH);
    char *kernel_source = read_source(VECTORDB_KERNEL_SOURCE_PATH);
    size_t i;

    if (source == NULL || kernel_source == NULL) {
        free(source);
        free(kernel_source);
        return 1;
    }
    for (i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (!contains(source, required[i])) {
            free(source);
            free(kernel_source);
            return 1;
        }
    }
    for (i = 0; i < sizeof(kernel_required) / sizeof(kernel_required[0]); ++i) {
        if (!contains(kernel_source, kernel_required[i])) {
            free(source);
            free(kernel_source);
            return 1;
        }
    }
    if (!rejects(source, "options->queries, 1, 1, 1, 1, 1") || !rejects(source, "kExactL2Top10Ptx") ||
        !rejects(source, "double qps = kernel_ms") || !contains(source, "double qps = end_to_end_ms > 0.0") ||
        !contains_at_least(source, "record_coherent_grant(&grant_evidence", 2) ||
        !ordered(source, "grant->lines_granted = device_lines_granted;", "++grant->partial_grants;") ||
        !ordered(source, "record_coherent_grant(&grant_evidence, post_update_lines_granted)", "emit_result(&options") ||
        !ordered(source, "cuMemcpyDtoH_v2(query_elapsed_ns", "epoch_end = now_ms();") ||
        !ordered(source, "epoch_end = now_ms();", "oracle_start = now_ms();") ||
        !ordered(source, "oracle_end = now_ms();", "emit_result(&options")) {
        free(source);
        free(kernel_source);
        return 1;
    }
    if (!contains_at_least(source, "cuLaunchKernel", 3)) {
        free(source);
        free(kernel_source);
        return 1;
    }
    free(source);
    free(kernel_source);
    return 0;
}
