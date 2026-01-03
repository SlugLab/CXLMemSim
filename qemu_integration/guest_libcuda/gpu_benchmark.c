/*
 * GPU Compute Benchmark for CXL Type 2 Device
 * Tests PTX loading and kernel execution through hetGPU backend
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* CUDA types */
typedef void *CUcontext;
typedef void *CUdevice;
typedef void *CUmodule;
typedef void *CUfunction;
typedef uint64_t CUdeviceptr;
typedef int CUresult;

#define CUDA_SUCCESS 0

/* CUDA function declarations */
extern CUresult cuInit(unsigned int flags);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuCtxCreate(CUcontext *ctx, unsigned int flags, CUdevice dev);
extern CUresult cuCtxDestroy(CUcontext ctx);
extern CUresult cuCtxSynchronize(void);
extern CUresult cuMemAlloc(CUdeviceptr *dptr, size_t bytesize);
extern CUresult cuMemFree(CUdeviceptr dptr);
extern CUresult cuMemcpyHtoD(CUdeviceptr dstDevice, const void *srcHost, size_t byteCount);
extern CUresult cuMemcpyDtoH(void *dstHost, CUdeviceptr srcDevice, size_t byteCount);
extern CUresult cuModuleLoadData(CUmodule *module, const void *image);
extern CUresult cuModuleGetFunction(CUfunction *hfunc, CUmodule hmod, const char *name);
extern CUresult cuLaunchKernel(CUfunction f,
                               unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
                               unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
                               unsigned int sharedMemBytes, void *hStream,
                               void **kernelParams, void **extra);

/* Simple vector add PTX kernel - PTX 8.0 required for sm_90 (H100) */
static const char *vector_add_ptx =
    ".version 8.0\n"
    ".target sm_90\n"
    ".address_size 64\n"
    "\n"
    ".visible .entry vector_add(\n"
    "    .param .u64 a,\n"
    "    .param .u64 b,\n"
    "    .param .u64 c,\n"
    "    .param .u32 n\n"
    ")\n"
    "{\n"
    "    .reg .pred %p<2>;\n"
    "    .reg .f32 %f<4>;\n"
    "    .reg .b32 %r<5>;\n"
    "    .reg .b64 %rd<11>;\n"
    "\n"
    "    ld.param.u64 %rd1, [a];\n"
    "    ld.param.u64 %rd2, [b];\n"
    "    ld.param.u64 %rd3, [c];\n"
    "    ld.param.u32 %r1, [n];\n"
    "    mov.u32 %r2, %ctaid.x;\n"
    "    mov.u32 %r3, %ntid.x;\n"
    "    mov.u32 %r4, %tid.x;\n"
    "    mad.lo.s32 %r2, %r3, %r2, %r4;\n"
    "    setp.ge.s32 %p1, %r2, %r1;\n"
    "    @%p1 bra $L__BB0_2;\n"
    "\n"
    "    cvta.to.global.u64 %rd4, %rd1;\n"
    "    mul.wide.s32 %rd5, %r2, 4;\n"
    "    add.s64 %rd6, %rd4, %rd5;\n"
    "    cvta.to.global.u64 %rd7, %rd2;\n"
    "    add.s64 %rd8, %rd7, %rd5;\n"
    "    ld.global.f32 %f1, [%rd6];\n"
    "    ld.global.f32 %f2, [%rd8];\n"
    "    add.f32 %f3, %f1, %f2;\n"
    "    cvta.to.global.u64 %rd9, %rd3;\n"
    "    add.s64 %rd10, %rd9, %rd5;\n"
    "    st.global.f32 [%rd10], %f3;\n"
    "\n"
    "$L__BB0_2:\n"
    "    ret;\n"
    "}\n";

/* Matrix multiply PTX kernel (simple) - PTX 8.0 required for sm_90 (H100) */
static const char *matmul_ptx =
    ".version 8.0\n"
    ".target sm_90\n"
    ".address_size 64\n"
    "\n"
    ".visible .entry matmul(\n"
    "    .param .u64 A,\n"
    "    .param .u64 B,\n"
    "    .param .u64 C,\n"
    "    .param .u32 N\n"
    ")\n"
    "{\n"
    "    .reg .pred %p<2>;\n"
    "    .reg .f32 %f<4>;\n"
    "    .reg .b32 %r<10>;\n"
    "    .reg .b64 %rd<20>;\n"
    "\n"
    "    ld.param.u64 %rd1, [A];\n"
    "    ld.param.u64 %rd2, [B];\n"
    "    ld.param.u64 %rd3, [C];\n"
    "    ld.param.u32 %r1, [N];\n"
    "    mov.u32 %r2, %ctaid.x;\n"
    "    mov.u32 %r3, %ctaid.y;\n"
    "    mov.u32 %r4, %ntid.x;\n"
    "    mov.u32 %r5, %tid.x;\n"
    "    mov.u32 %r6, %tid.y;\n"
    "    mad.lo.s32 %r7, %r4, %r2, %r5;\n"  /* col */
    "    mad.lo.s32 %r8, %r4, %r3, %r6;\n"  /* row */
    "    setp.ge.s32 %p1, %r7, %r1;\n"
    "    @%p1 bra $L__END;\n"
    "    setp.ge.s32 %p1, %r8, %r1;\n"
    "    @%p1 bra $L__END;\n"
    "\n"
    "    /* C[row*N+col] = A[row*N+0]*B[0*N+col] (simplified) */\n"
    "    cvta.to.global.u64 %rd4, %rd3;\n"
    "    mul.lo.s32 %r9, %r8, %r1;\n"
    "    add.s32 %r9, %r9, %r7;\n"
    "    mul.wide.s32 %rd5, %r9, 4;\n"
    "    add.s64 %rd6, %rd4, %rd5;\n"
    "    mov.f32 %f1, 1.0;\n"  /* Placeholder result */
    "    st.global.f32 [%rd6], %f1;\n"
    "\n"
    "$L__END:\n"
    "    ret;\n"
    "}\n";

static double get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static int check_cuda(CUresult err, const char *call, int line)
{
    if (err != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA Error %d at line %d: %s\n", err, line, call);
        return -1;
    }
    return 0;
}

#define CHECK_CUDA(call) if (check_cuda((call), #call, __LINE__)) return -1

int benchmark_vector_add(int n, int iterations)
{
    CUdevice device;
    CUcontext ctx;
    CUmodule module;
    CUfunction func;
    CUdeviceptr d_a, d_b, d_c;
    float *h_a, *h_b, *h_c;
    size_t size = n * sizeof(float);
    double start, end, total_time = 0;
    int i, errors = 0;

    printf("\n=== Vector Add Benchmark ===\n");
    printf("Vector size: %d elements (%zu KB)\n", n, size / 1024);
    printf("Iterations: %d\n", iterations);

    /* Allocate host memory */
    h_a = (float *)malloc(size);
    h_b = (float *)malloc(size);
    h_c = (float *)malloc(size);
    if (!h_a || !h_b || !h_c) {
        fprintf(stderr, "Host malloc failed\n");
        return -1;
    }

    /* Initialize data */
    for (i = 0; i < n; i++) {
        h_a[i] = (float)i;
        h_b[i] = (float)(i * 2);
    }

    /* Initialize CUDA */
    CHECK_CUDA(cuInit(0));
    CHECK_CUDA(cuDeviceGet(&device, 0));
    CHECK_CUDA(cuCtxCreate(&ctx, 0, device));

    /* Load PTX module */
    printf("Loading PTX module...\n");
    CHECK_CUDA(cuModuleLoadData(&module, vector_add_ptx));
    CHECK_CUDA(cuModuleGetFunction(&func, module, "vector_add"));
    printf("  Module loaded, function: %p\n", func);

    /* Allocate device memory */
    CHECK_CUDA(cuMemAlloc(&d_a, size));
    CHECK_CUDA(cuMemAlloc(&d_b, size));
    CHECK_CUDA(cuMemAlloc(&d_c, size));

    /* Copy input data */
    CHECK_CUDA(cuMemcpyHtoD(d_a, h_a, size));
    CHECK_CUDA(cuMemcpyHtoD(d_b, h_b, size));

    /* Run benchmark */
    printf("Running kernel...\n");
    int threads = 256;
    int blocks = (n + threads - 1) / threads;

    for (i = 0; i < iterations; i++) {
        void *args[] = { &d_a, &d_b, &d_c, &n, NULL };

        start = get_time_ms();
        CHECK_CUDA(cuLaunchKernel(func, blocks, 1, 1, threads, 1, 1, 0, NULL, args, NULL));
        CHECK_CUDA(cuCtxSynchronize());
        end = get_time_ms();

        total_time += (end - start);
    }

    /* Copy result back */
    CHECK_CUDA(cuMemcpyDtoH(h_c, d_c, size));

    /* Verify results */
    for (i = 0; i < n; i++) {
        float expected = h_a[i] + h_b[i];
        if (h_c[i] != expected) {
            if (errors < 5) {
                printf("  Mismatch at %d: got %f, expected %f\n", i, h_c[i], expected);
            }
            errors++;
        }
    }

    printf("\nResults:\n");
    printf("  Average kernel time: %.3f ms\n", total_time / iterations);
    printf("  Throughput: %.2f GB/s\n", (3.0 * size / (1024*1024*1024)) / (total_time / iterations / 1000));
    printf("  Verification: %s (%d errors)\n", errors == 0 ? "PASSED" : "FAILED", errors);

    /* Cleanup */
    cuMemFree(d_a);
    cuMemFree(d_b);
    cuMemFree(d_c);
    cuCtxDestroy(ctx);
    free(h_a);
    free(h_b);
    free(h_c);

    return errors == 0 ? 0 : -1;
}

int benchmark_memory_bandwidth(size_t size, int iterations)
{
    CUdevice device;
    CUcontext ctx;
    CUdeviceptr d_buf;
    void *h_buf;
    double start, end;
    double htod_time = 0, dtoh_time = 0;
    int i;

    printf("\n=== Memory Bandwidth Benchmark ===\n");
    printf("Buffer size: %zu MB\n", size / (1024 * 1024));
    printf("Iterations: %d\n", iterations);

    /* Allocate host memory */
    h_buf = malloc(size);
    if (!h_buf) {
        fprintf(stderr, "Host malloc failed\n");
        return -1;
    }
    memset(h_buf, 0xAB, size);

    /* Initialize CUDA */
    CHECK_CUDA(cuInit(0));
    CHECK_CUDA(cuDeviceGet(&device, 0));
    CHECK_CUDA(cuCtxCreate(&ctx, 0, device));

    /* Allocate device memory */
    CHECK_CUDA(cuMemAlloc(&d_buf, size));

    /* Benchmark HtoD */
    printf("Testing Host-to-Device...\n");
    for (i = 0; i < iterations; i++) {
        start = get_time_ms();
        CHECK_CUDA(cuMemcpyHtoD(d_buf, h_buf, size));
        end = get_time_ms();
        htod_time += (end - start);
    }

    /* Benchmark DtoH */
    printf("Testing Device-to-Host...\n");
    for (i = 0; i < iterations; i++) {
        start = get_time_ms();
        CHECK_CUDA(cuMemcpyDtoH(h_buf, d_buf, size));
        end = get_time_ms();
        dtoh_time += (end - start);
    }

    printf("\nResults:\n");
    printf("  HtoD: %.2f GB/s (%.3f ms avg)\n",
           (size / (1024.0*1024*1024)) / (htod_time / iterations / 1000),
           htod_time / iterations);
    printf("  DtoH: %.2f GB/s (%.3f ms avg)\n",
           (size / (1024.0*1024*1024)) / (dtoh_time / iterations / 1000),
           dtoh_time / iterations);

    /* Cleanup */
    cuMemFree(d_buf);
    cuCtxDestroy(ctx);
    free(h_buf);

    return 0;
}

int main(int argc, char **argv)
{
    int ret = 0;

    printf("CXL Type 2 GPU Benchmark Suite\n");
    printf("==============================\n");

    /* Memory bandwidth test */
    ret |= benchmark_memory_bandwidth(64 * 1024 * 1024, 10);  /* 64MB, 10 iterations */

    /* Vector add test */
    ret |= benchmark_vector_add(1024 * 1024, 10);  /* 1M elements, 10 iterations */

    printf("\n==============================\n");
    printf("Benchmark %s\n", ret == 0 ? "PASSED" : "FAILED");

    return ret;
}
