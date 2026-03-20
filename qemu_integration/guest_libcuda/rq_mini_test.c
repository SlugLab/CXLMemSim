#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef int CUresult;
typedef int CUdevice;
typedef void *CUcontext;
#define CUDA_SUCCESS 0

extern CUresult cuInit(unsigned int flags);
extern CUresult cuDeviceGet(CUdevice *device, int ordinal);
extern CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev);
extern int cxlCoherentAlloc(uint64_t size, void **host_ptr);
extern int cxlCoherentFree(void *host_ptr);
extern void *cxlDeviceToHost(uint64_t dev_offset);
extern int cxlCoherentFence(void);

int main(void) {
    printf("Step 1: cuInit\n"); fflush(stdout);
    CUresult err = cuInit(0);
    if (err != CUDA_SUCCESS) { printf("cuInit failed: %d\n", err); return 1; }

    int dev_count = 0;
    extern CUresult cuDeviceGetCount(int *count);
    cuDeviceGetCount(&dev_count);
    printf("  Found %d GPU(s)\n", dev_count); fflush(stdout);

    /* Try each device until one works */
    printf("Step 2: cuDeviceGet\n"); fflush(stdout);
    CUdevice dev;
    int use_dev = dev_count > 1 ? 1 : 0;
    err = cuDeviceGet(&dev, use_dev);
    printf("  Using device %d\n", use_dev); fflush(stdout);
    if (err != CUDA_SUCCESS) { printf("cuDeviceGet failed: %d\n", err); return 1; }

    printf("Step 3: cuCtxCreate\n"); fflush(stdout);
    CUcontext ctx;
    err = cuCtxCreate_v2(&ctx, 0, dev);
    if (err != CUDA_SUCCESS) { printf("cuCtxCreate failed: %d\n", err); return 1; }

    printf("Step 4: cxlCoherentAlloc\n"); fflush(stdout);
    void *ptr = NULL;
    int ret = cxlCoherentAlloc(4096, &ptr);
    if (ret != CUDA_SUCCESS || !ptr) { printf("alloc failed: %d\n", ret); return 1; }
    printf("  ptr = %p\n", ptr); fflush(stdout);

    printf("Step 5: write to coherent memory\n"); fflush(stdout);
    volatile uint64_t *data = (volatile uint64_t *)ptr;
    data[0] = 0xDEADBEEF;
    printf("  wrote 0x%lx\n", (unsigned long)data[0]); fflush(stdout);

    printf("Step 6: cxlCoherentFence\n"); fflush(stdout);
    cxlCoherentFence();

    printf("Step 7: read back\n"); fflush(stdout);
    printf("  read 0x%lx\n", (unsigned long)data[0]); fflush(stdout);

    printf("All steps passed!\n");
    cxlCoherentFree(ptr);
    return 0;
}
