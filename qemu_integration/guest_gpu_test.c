/*
 * CXL Type 2 GPU Test Program
 * Tests the hetGPU backend through CXL Type 2 device
 *
 * Compile: gcc -o guest_gpu_test guest_gpu_test.c -ldl
 * Run: ./guest_gpu_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <dlfcn.h>

/* CXL Type 2 device identifiers */
#define CXL_TYPE2_VENDOR_ID 0x8086
#define CXL_TYPE2_DEVICE_ID 0x0d92

/* CUDA-like types */
typedef int CUresult;
typedef void* CUdevice;
typedef void* CUcontext;
typedef void* CUmodule;
typedef void* CUfunction;
typedef uint64_t CUdeviceptr;

#define CUDA_SUCCESS 0

/* Function pointer types */
typedef CUresult (*cuInit_t)(unsigned int);
typedef CUresult (*cuDeviceGetCount_t)(int*);
typedef CUresult (*cuDeviceGet_t)(CUdevice*, int);
typedef CUresult (*cuDeviceGetName_t)(char*, int, CUdevice);
typedef CUresult (*cuDeviceTotalMem_t)(size_t*, CUdevice);
typedef CUresult (*cuCtxCreate_t)(CUcontext*, unsigned int, CUdevice);
typedef CUresult (*cuMemAlloc_t)(CUdeviceptr*, size_t);
typedef CUresult (*cuMemFree_t)(CUdeviceptr);
typedef CUresult (*cuMemcpyHtoD_t)(CUdeviceptr, const void*, size_t);
typedef CUresult (*cuMemcpyDtoH_t)(void*, CUdeviceptr, size_t);

/* Test using direct PCI access */
int test_pci_device(void)
{
    char path[256];
    int fd;
    uint16_t vendor, device;

    printf("=== Testing PCI Device Access ===\n");

    /* Try to find the CXL Type 2 device */
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            snprintf(path, sizeof(path),
                     "/sys/bus/pci/devices/0000:%02x:%02x.0/vendor", bus, dev);
            fd = open(path, O_RDONLY);
            if (fd < 0) continue;

            char buf[16];
            if (read(fd, buf, sizeof(buf)) > 0) {
                vendor = strtol(buf, NULL, 16);
                close(fd);

                snprintf(path, sizeof(path),
                         "/sys/bus/pci/devices/0000:%02x:%02x.0/device", bus, dev);
                fd = open(path, O_RDONLY);
                if (fd >= 0 && read(fd, buf, sizeof(buf)) > 0) {
                    device = strtol(buf, NULL, 16);
                    close(fd);

                    if (vendor == CXL_TYPE2_VENDOR_ID && device == CXL_TYPE2_DEVICE_ID) {
                        printf("Found CXL Type 2 device at %02x:%02x.0\n", bus, dev);
                        printf("  Vendor: 0x%04x, Device: 0x%04x\n", vendor, device);

                        /* Read resource info */
                        snprintf(path, sizeof(path),
                                 "/sys/bus/pci/devices/0000:%02x:%02x.0/resource", bus, dev);
                        fd = open(path, O_RDONLY);
                        if (fd >= 0) {
                            char resource[1024];
                            ssize_t n = read(fd, resource, sizeof(resource)-1);
                            if (n > 0) {
                                resource[n] = '\0';
                                printf("  Resources:\n%s", resource);
                            }
                            close(fd);
                        }
                        return 0;
                    }
                }
            }
            close(fd);
        }
    }

    printf("CXL Type 2 device not found\n");
    return -1;
}

/* Test using CUDA driver API (if available) */
int test_cuda_api(void)
{
    void *handle;
    cuInit_t cuInit;
    cuDeviceGetCount_t cuDeviceGetCount;
    cuDeviceGet_t cuDeviceGet;
    cuDeviceGetName_t cuDeviceGetName;
    cuDeviceTotalMem_t cuDeviceTotalMem;
    cuCtxCreate_t cuCtxCreate;
    cuMemAlloc_t cuMemAlloc;
    cuMemFree_t cuMemFree;
    cuMemcpyHtoD_t cuMemcpyHtoD;
    cuMemcpyDtoH_t cuMemcpyDtoH;

    int count;
    CUdevice dev;
    CUcontext ctx;
    CUdeviceptr devPtr;
    char name[256];
    size_t totalMem;
    CUresult err;

    printf("\n=== Testing CUDA API ===\n");

    /* Try to load CUDA library */
    handle = dlopen("libcuda.so.1", RTLD_NOW);
    if (!handle) {
        handle = dlopen("libcuda.so", RTLD_NOW);
    }
    if (!handle) {
        handle = dlopen("libnvcuda.so", RTLD_NOW);
    }
    if (!handle) {
        printf("Could not load CUDA library: %s\n", dlerror());
        printf("This is expected if CUDA is not installed in guest\n");
        return -1;
    }

    printf("CUDA library loaded successfully\n");

    /* Load functions */
    cuInit = (cuInit_t)dlsym(handle, "cuInit");
    cuDeviceGetCount = (cuDeviceGetCount_t)dlsym(handle, "cuDeviceGetCount");
    cuDeviceGet = (cuDeviceGet_t)dlsym(handle, "cuDeviceGet");
    cuDeviceGetName = (cuDeviceGetName_t)dlsym(handle, "cuDeviceGetName");
    cuDeviceTotalMem = (cuDeviceTotalMem_t)dlsym(handle, "cuDeviceTotalMem_v2");
    cuCtxCreate = (cuCtxCreate_t)dlsym(handle, "cuCtxCreate_v2");
    cuMemAlloc = (cuMemAlloc_t)dlsym(handle, "cuMemAlloc_v2");
    cuMemFree = (cuMemFree_t)dlsym(handle, "cuMemFree_v2");
    cuMemcpyHtoD = (cuMemcpyHtoD_t)dlsym(handle, "cuMemcpyHtoD_v2");
    cuMemcpyDtoH = (cuMemcpyDtoH_t)dlsym(handle, "cuMemcpyDtoH_v2");

    if (!cuInit) {
        printf("Could not find cuInit\n");
        dlclose(handle);
        return -1;
    }

    /* Initialize CUDA */
    err = cuInit(0);
    if (err != CUDA_SUCCESS) {
        printf("cuInit failed: %d\n", err);
        dlclose(handle);
        return -1;
    }
    printf("CUDA initialized\n");

    /* Get device count */
    if (cuDeviceGetCount) {
        err = cuDeviceGetCount(&count);
        if (err == CUDA_SUCCESS) {
            printf("Device count: %d\n", count);
        }
    }

    /* Get device info */
    if (cuDeviceGet && count > 0) {
        err = cuDeviceGet(&dev, 0);
        if (err == CUDA_SUCCESS) {
            printf("Got device 0\n");

            if (cuDeviceGetName) {
                err = cuDeviceGetName(name, sizeof(name), dev);
                if (err == CUDA_SUCCESS) {
                    printf("Device name: %s\n", name);
                }
            }

            if (cuDeviceTotalMem) {
                err = cuDeviceTotalMem(&totalMem, dev);
                if (err == CUDA_SUCCESS) {
                    printf("Total memory: %zu MB\n", totalMem / (1024*1024));
                }
            }
        }
    }

    /* Test memory allocation */
    if (cuCtxCreate && cuMemAlloc && cuMemFree && count > 0) {
        printf("\n=== Testing Memory Operations ===\n");

        err = cuCtxCreate(&ctx, 0, dev);
        if (err == CUDA_SUCCESS) {
            printf("Context created\n");

            /* Allocate device memory */
            size_t size = 1024 * 1024; /* 1 MB */
            err = cuMemAlloc(&devPtr, size);
            if (err == CUDA_SUCCESS) {
                printf("Allocated %zu bytes at device address 0x%lx\n", size, (unsigned long)devPtr);

                /* Test memcpy if available */
                if (cuMemcpyHtoD && cuMemcpyDtoH) {
                    char *hostBuf = malloc(size);
                    char *resultBuf = malloc(size);

                    if (hostBuf && resultBuf) {
                        /* Fill with pattern */
                        memset(hostBuf, 0xAB, size);

                        /* Copy to device */
                        err = cuMemcpyHtoD(devPtr, hostBuf, size);
                        if (err == CUDA_SUCCESS) {
                            printf("Host to device copy succeeded\n");

                            /* Copy back */
                            memset(resultBuf, 0, size);
                            err = cuMemcpyDtoH(resultBuf, devPtr, size);
                            if (err == CUDA_SUCCESS) {
                                printf("Device to host copy succeeded\n");

                                /* Verify */
                                if (memcmp(hostBuf, resultBuf, size) == 0) {
                                    printf("Data verification PASSED!\n");
                                } else {
                                    printf("Data verification FAILED!\n");
                                }
                            }
                        }

                        free(hostBuf);
                        free(resultBuf);
                    }
                }

                cuMemFree(devPtr);
                printf("Memory freed\n");
            } else {
                printf("Memory allocation failed: %d\n", err);
            }
        } else {
            printf("Context creation failed: %d\n", err);
        }
    }

    dlclose(handle);
    return 0;
}

/* Test CXL memory region directly */
int test_cxl_memory(void)
{
    char path[256];
    int fd;
    void *map;
    uint64_t start, end, flags;

    printf("\n=== Testing CXL Memory Region ===\n");

    /* Find the device's BAR0 */
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/0000:0d:00.0/resource0");
    fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) {
        printf("Could not open resource0: %s\n", path);
        printf("Try running as root\n");
        return -1;
    }

    /* Get resource size from sysfs */
    FILE *fp = fopen("/sys/bus/pci/devices/0000:0d:00.0/resource", "r");
    if (fp) {
        if (fscanf(fp, "0x%lx 0x%lx 0x%lx", &start, &end, &flags) == 3) {
            size_t size = end - start + 1;
            printf("BAR0: start=0x%lx end=0x%lx size=%zu\n", start, end, size);

            if (size > 0 && size < 1024*1024*1024) {
                map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (map != MAP_FAILED) {
                    printf("Mapped BAR0 at %p\n", map);

                    /* Read some registers */
                    uint32_t *regs = (uint32_t *)map;
                    printf("Register[0]: 0x%08x\n", regs[0]);
                    printf("Register[1]: 0x%08x\n", regs[1]);
                    printf("Register[2]: 0x%08x\n", regs[2]);
                    printf("Register[3]: 0x%08x\n", regs[3]);

                    munmap(map, size);
                } else {
                    perror("mmap failed");
                }
            }
        }
        fclose(fp);
    }

    close(fd);
    return 0;
}

int main(int argc, char *argv[])
{
    printf("CXL Type 2 GPU Test Program\n");
    printf("============================\n\n");

    /* Test PCI device access */
    test_pci_device();

    /* Test CXL memory region */
    test_cxl_memory();

    /* Test CUDA API */
    test_cuda_api();

    printf("\n=== Test Complete ===\n");
    return 0;
}
