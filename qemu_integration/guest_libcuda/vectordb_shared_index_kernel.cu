#include <cuda_runtime.h>
#include <limits.h>
#include <stdint.h>

enum {
    kThreads = 256,
    kWarpSize = 32,
    kRowsPerBlock = 8,
    kTopK = 10,
};

static __device__ __forceinline__ unsigned long long read_globaltimer(void) {
    unsigned long long value;

    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(value));
    return value;
}

static __device__ __forceinline__ bool candidate_precedes(float left_distance, int left_label, float right_distance,
                                                          int right_label) {
    return left_distance < right_distance || (left_distance == right_distance && left_label < right_label);
}

static __device__ __forceinline__ void insert_candidate(float distance, int label, float *best_distances,
                                                        int *best_labels) {
    for (int position = 0; position < kTopK; ++position) {
        if (candidate_precedes(distance, label, best_distances[position], best_labels[position])) {
            for (int move = kTopK - 1; move > position; --move) {
                best_distances[move] = best_distances[move - 1];
                best_labels[move] = best_labels[move - 1];
            }
            best_distances[position] = distance;
            best_labels[position] = label;
            return;
        }
    }
}

extern "C" __global__ void vectordb_distance_tiled(const float *index, const float *queries, uint32_t rows,
                                                   uint32_t dim, float *distance_scratch,
                                                   unsigned long long *query_start_ns) {
    const uint32_t query = blockIdx.y;
    const uint32_t warp = threadIdx.x / 32;
    const uint32_t lane = threadIdx.x % 32;
    const uint32_t row = blockIdx.x * kRowsPerBlock + warp;

    if (threadIdx.x == 0) {
        atomicMin(query_start_ns + query, read_globaltimer());
    }
    if (row >= rows) {
        return;
    }

    float sum = 0.0f;
    const size_t row_base = (size_t)row * dim;
    const size_t query_base = (size_t)query * dim;
    for (uint32_t dimension = lane; dimension < dim; dimension += 32) {
        const float delta = index[row_base + dimension] - queries[query_base + dimension];
        sum = __fadd_rn(sum, __fmul_rn(delta, delta));
    }
    for (uint32_t offset = kWarpSize / 2; offset != 0; offset /= 2) {
        sum = __fadd_rn(sum, __shfl_down_sync(0xffffffffU, sum, offset));
    }
    if (lane == 0) {
        distance_scratch[(size_t)query * rows + row] = sum;
    }
}

extern "C" __global__ void vectordb_topk_tiled(const float *distance_scratch, uint32_t rows, uint32_t topk, int *labels,
                                               float *distances, const unsigned long long *query_start_ns,
                                               unsigned long long *query_elapsed_ns) {
    __shared__ float candidate_distances[256 * 10];
    __shared__ int candidate_labels[256 * 10];
    const uint32_t query = blockIdx.x;
    const uint32_t thread = threadIdx.x;
    float local_distances[kTopK];
    int local_labels[kTopK];

    if (topk != kTopK || blockDim.x != kThreads) {
        return;
    }
    for (int item = 0; item < kTopK; ++item) {
        local_distances[item] = __int_as_float(0x7f800000);
        local_labels[item] = INT_MAX;
    }
    for (uint32_t row = thread; row < rows; row += blockDim.x) {
        insert_candidate(distance_scratch[(size_t)query * rows + row], (int)row, local_distances, local_labels);
    }
    for (int item = 0; item < kTopK; ++item) {
        const uint32_t candidate = thread * kTopK + (uint32_t)item;
        candidate_distances[candidate] = local_distances[item];
        candidate_labels[candidate] = local_labels[item];
    }
    __syncthreads();

    if (thread == 0) {
        float best_distances[kTopK];
        int best_labels[kTopK];

        for (int item = 0; item < kTopK; ++item) {
            best_distances[item] = __int_as_float(0x7f800000);
            best_labels[item] = INT_MAX;
        }
        for (uint32_t candidate = 0; candidate < kThreads * kTopK; ++candidate) {
            insert_candidate(candidate_distances[candidate], candidate_labels[candidate], best_distances, best_labels);
        }
        for (int item = 0; item < kTopK; ++item) {
            const size_t output = (size_t)query * kTopK + (size_t)item;
            distances[output] = best_distances[item];
            labels[output] = best_labels[item];
        }
        query_elapsed_ns[query] = read_globaltimer() - query_start_ns[query];
    }
}
