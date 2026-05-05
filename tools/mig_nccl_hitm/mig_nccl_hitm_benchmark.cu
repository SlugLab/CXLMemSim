/*
 * MIG/NCCL GPU-side HITM-style handoff benchmark.
 *
 * This is the GPU-side companion for
 * qemu_integration/guest_libcuda/cpu_gpu_hitm_benchmark.c.  It measures a
 * cache-line-sized token handoff across the CUDA devices visible to this
 * process, which can be MIG compute instances when CUDA_VISIBLE_DEVICES is set
 * accordingly.  Build it against vickiegpt/nccl-mig, or any NCCL-compatible
 * build, and run with NCCL_BYPASS_MIG_DUPLICATE_CHECK=1 for same-GPU MIG ranks.
 */

#include <cuda_runtime.h>
#include <nccl.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct __align__(64) SharedLine {
    uint32_t counter;
    uint32_t producer_rank;
    uint32_t seq;
    uint32_t checksum;
    uint8_t pad[48];
};

static uint64_t time_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int parse_int_arg(const char *arg, int fallback)
{
    char *end = NULL;
    long value;

    if (arg == NULL || *arg == '\0') {
        return fallback;
    }
    value = strtol(arg, &end, 10);
    if (end == arg || *end != '\0' || value <= 0 || value > 100000000L) {
        return fallback;
    }
    return (int)value;
}

static int check_cuda(cudaError_t err, const char *call, int line)
{
    if (err != cudaSuccess) {
        fprintf(stderr, "CUDA error at %s:%d: %s returned %s\n",
                __FILE__, line, call, cudaGetErrorString(err));
        return -1;
    }
    return 0;
}

static int check_nccl(ncclResult_t err, const char *call, int line)
{
    if (err != ncclSuccess) {
        fprintf(stderr, "NCCL error at %s:%d: %s returned %s\n",
                __FILE__, line, call, ncclGetErrorString(err));
        return -1;
    }
    return 0;
}

#define CHECK_CUDA(call) do { if (check_cuda((call), #call, __LINE__) != 0) return 1; } while (0)
#define CHECK_NCCL(call) do { if (check_nccl((call), #call, __LINE__) != 0) return 1; } while (0)

__global__ void produce_lines(SharedLine *lines, int line_count, uint32_t seq, uint32_t rank)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= line_count) {
        return;
    }
    lines[idx].counter += 1;
    lines[idx].producer_rank = rank;
    lines[idx].seq = seq;
    lines[idx].checksum = lines[idx].counter ^ seq ^ rank;
}

__global__ void consume_lines(SharedLine *lines, int line_count, uint32_t seq, uint32_t expected_rank)
{
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t counter;

    if (idx >= line_count) {
        return;
    }
    counter = lines[idx].counter + 1;
    lines[idx].counter = counter;
    lines[idx].producer_rank = expected_rank;
    lines[idx].seq = seq;
    lines[idx].checksum = counter ^ seq ^ expected_rank;
}

static int validate_rank(int dev, const SharedLine *d_lines, int line_count, uint32_t seq, uint32_t expected_rank)
{
    SharedLine sample;

    CHECK_CUDA(cudaSetDevice(dev));
    CHECK_CUDA(cudaMemcpy(&sample, d_lines, sizeof(sample), cudaMemcpyDeviceToHost));
    if (sample.seq != seq || sample.producer_rank != expected_rank ||
        sample.checksum != (sample.counter ^ seq ^ expected_rank)) {
        fprintf(stderr,
                "Validation failed on device %d: counter=%u rank=%u seq=%u checksum=%u expected rank=%u seq=%u\n",
                dev, sample.counter, sample.producer_rank, sample.seq, sample.checksum, expected_rank, seq);
        return 1;
    }
    (void)line_count;
    return 0;
}

int main(int argc, char **argv)
{
    int line_count = parse_int_arg(argc > 1 ? argv[1] : NULL, 64);
    int iterations = parse_int_arg(argc > 2 ? argv[2] : NULL, 1000);
    int warmup = parse_int_arg(argc > 3 ? argv[3] : NULL, 100);
    int requested_devs = parse_int_arg(argc > 4 ? argv[4] : NULL, 0);
    int visible_devs = 0;
    int ndevs;
    size_t bytes = (size_t)line_count * sizeof(SharedLine);
    int *devs = NULL;
    ncclComm_t *comms = NULL;
    cudaStream_t *streams = NULL;
    SharedLine **send_lines = NULL;
    SharedLine **recv_lines = NULL;
    uint64_t total_ns = 0;
    int measured = 0;

    CHECK_CUDA(cudaGetDeviceCount(&visible_devs));
    if (visible_devs <= 0) {
        fprintf(stderr, "No CUDA devices are visible\n");
        return 1;
    }
    ndevs = requested_devs > 0 && requested_devs < visible_devs ? requested_devs : visible_devs;

    devs = (int *)calloc((size_t)ndevs, sizeof(*devs));
    comms = (ncclComm_t *)calloc((size_t)ndevs, sizeof(*comms));
    streams = (cudaStream_t *)calloc((size_t)ndevs, sizeof(*streams));
    send_lines = (SharedLine **)calloc((size_t)ndevs, sizeof(*send_lines));
    recv_lines = (SharedLine **)calloc((size_t)ndevs, sizeof(*recv_lines));
    if (!devs || !comms || !streams || !send_lines || !recv_lines) {
        fprintf(stderr, "Allocation failed\n");
        return 1;
    }

    for (int i = 0; i < ndevs; i++) {
        devs[i] = i;
        CHECK_CUDA(cudaSetDevice(devs[i]));
        CHECK_CUDA(cudaStreamCreateWithFlags(&streams[i], cudaStreamNonBlocking));
        CHECK_CUDA(cudaMalloc(&send_lines[i], bytes));
        CHECK_CUDA(cudaMalloc(&recv_lines[i], bytes));
        CHECK_CUDA(cudaMemsetAsync(send_lines[i], 0, bytes, streams[i]));
        CHECK_CUDA(cudaMemsetAsync(recv_lines[i], 0, bytes, streams[i]));
    }
    for (int i = 0; i < ndevs; i++) {
        CHECK_CUDA(cudaSetDevice(devs[i]));
        CHECK_CUDA(cudaStreamSynchronize(streams[i]));
    }

    CHECK_NCCL(ncclCommInitAll(comms, ndevs, devs));

    printf("MIG/NCCL HITM-style benchmark\n");
    printf("  devices=%d lines=%d iterations=%d warmup=%d bytes_per_rank=%zu\n",
           ndevs, line_count, iterations, warmup, bytes);

    for (int iter = 0; iter < warmup + iterations; iter++) {
        uint32_t seq = (uint32_t)(iter + 1);
        uint64_t start_ns;
        uint64_t end_ns;
        int blocks = (line_count + 127) / 128;

        for (int r = 0; r < ndevs; r++) {
            CHECK_CUDA(cudaSetDevice(devs[r]));
            produce_lines<<<blocks, 128, 0, streams[r]>>>(send_lines[r], line_count, seq, (uint32_t)r);
            CHECK_CUDA(cudaGetLastError());
        }

        start_ns = time_ns();
        CHECK_NCCL(ncclGroupStart());
        for (int r = 0; r < ndevs; r++) {
            int next = (r + 1) % ndevs;
            int prev = (r + ndevs - 1) % ndevs;
            CHECK_NCCL(ncclSend(send_lines[r], bytes, ncclUint8, next, comms[r], streams[r]));
            CHECK_NCCL(ncclRecv(recv_lines[r], bytes, ncclUint8, prev, comms[r], streams[r]));
        }
        CHECK_NCCL(ncclGroupEnd());

        for (int r = 0; r < ndevs; r++) {
            int prev = (r + ndevs - 1) % ndevs;
            CHECK_CUDA(cudaSetDevice(devs[r]));
            consume_lines<<<blocks, 128, 0, streams[r]>>>(recv_lines[r], line_count, seq, (uint32_t)prev);
            CHECK_CUDA(cudaGetLastError());
        }
        for (int r = 0; r < ndevs; r++) {
            CHECK_CUDA(cudaSetDevice(devs[r]));
            CHECK_CUDA(cudaStreamSynchronize(streams[r]));
        }
        end_ns = time_ns();

        if (iter >= warmup) {
            total_ns += end_ns - start_ns;
            measured++;
        }
    }

    for (int r = 0; r < ndevs; r++) {
        int prev = (r + ndevs - 1) % ndevs;
        if (validate_rank(devs[r], recv_lines[r], line_count, (uint32_t)(warmup + iterations), (uint32_t)prev) != 0) {
            return 1;
        }
    }

    printf("\n[nccl-mig-ring]\n");
    printf("  total_ns=%llu\n", (unsigned long long)total_ns);
    printf("  avg_roundtrip_ns=%.1f\n", measured ? (double)total_ns / (double)measured : 0.0);
    printf("  avg_line_handoff_ns=%.1f\n",
           (measured && line_count > 0 && ndevs > 0) ?
           (double)total_ns / ((double)measured * (double)line_count * (double)ndevs) : 0.0);
    printf("  handoffs_per_sec=%.2f\n",
           total_ns ? ((double)measured * (double)line_count * (double)ndevs * 1.0e9) / (double)total_ns : 0.0);

    for (int r = 0; r < ndevs; r++) {
        if (comms[r]) ncclCommDestroy(comms[r]);
        CHECK_CUDA(cudaSetDevice(devs[r]));
        if (send_lines[r]) cudaFree(send_lines[r]);
        if (recv_lines[r]) cudaFree(recv_lines[r]);
        if (streams[r]) cudaStreamDestroy(streams[r]);
    }
    free(devs);
    free(comms);
    free(streams);
    free(send_lines);
    free(recv_lines);
    return 0;
}
