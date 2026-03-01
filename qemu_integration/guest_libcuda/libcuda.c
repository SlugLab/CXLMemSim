/*
 * CXL Type 2 GPU - Guest libcuda.so Shim
 * Implements CUDA Driver API by communicating with CXL Type 2 device
 *
 * This library intercepts CUDA calls and forwards them to the CXL Type 2
 * device which runs the hetGPU backend on the host.
 *
 * Compile: gcc -shared -fPIC -o libcuda.so.1 libcuda.c -ldl
 * Usage: LD_PRELOAD=./libcuda.so.1 ./cuda_program
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "cxl_gpu_cmd.h"

/* CUDA types */
typedef int CUresult;
typedef int CUdevice;
typedef void* CUcontext;
typedef void* CUmodule;
typedef void* CUfunction;
typedef void* CUstream;
typedef void* CUevent;
typedef uint64_t CUdeviceptr;

/* CUDA error codes */
#define CUDA_SUCCESS                    0
#define CUDA_ERROR_INVALID_VALUE        1
#define CUDA_ERROR_OUT_OF_MEMORY        2
#define CUDA_ERROR_NOT_INITIALIZED      3
#define CUDA_ERROR_DEINITIALIZED        4
#define CUDA_ERROR_NO_DEVICE            100
#define CUDA_ERROR_INVALID_DEVICE       101
#define CUDA_ERROR_INVALID_CONTEXT      201
#define CUDA_ERROR_INVALID_HANDLE       400
#define CUDA_ERROR_NOT_FOUND            500
#define CUDA_ERROR_NOT_READY            600
#define CUDA_ERROR_LAUNCH_FAILED        700
#define CUDA_ERROR_UNKNOWN              999

/* CUDA device attributes */
#define CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK 1
#define CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X 2
#define CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Y 3
#define CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Z 4
#define CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X 5
#define CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Y 6
#define CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Z 7
#define CU_DEVICE_ATTRIBUTE_WARP_SIZE 10
#define CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT 16
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR 75
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR 76

/* CXL Type 2 device info */
#define CXL_TYPE2_VENDOR_ID 0x8086
#define CXL_TYPE2_DEVICE_ID 0x0d92

/* Global state */
static volatile uint32_t *g_regs = NULL;
static volatile uint8_t *g_data = NULL;
static int g_initialized = 0;
static int g_pci_fd = -1;
static size_t g_bar_size = 0;
static CUcontext g_context = NULL;

/* Debug logging */
static int g_debug = 0;
#define DLOG(...) do { if (g_debug) fprintf(stderr, "[CXL-CUDA] " __VA_ARGS__); } while(0)

/* Register access helpers */
static inline uint32_t reg_read32(uint32_t offset)
{
    return *(volatile uint32_t *)((uint8_t *)g_regs + offset);
}

static inline uint64_t reg_read64(uint32_t offset)
{
    return *(volatile uint64_t *)((uint8_t *)g_regs + offset);
}

static inline void reg_write32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)((uint8_t *)g_regs + offset) = value;
    __sync_synchronize();
}

static inline void reg_write64(uint32_t offset, uint64_t value)
{
    *(volatile uint64_t *)((uint8_t *)g_regs + offset) = value;
    __sync_synchronize();
}

static inline void data_write(size_t offset, const void *src, size_t len)
{
    if (offset + len <= CXL_GPU_DATA_SIZE) {
        const uint8_t *s = (const uint8_t *)src;
        volatile uint8_t *d = g_data + offset;
        /* Use volatile access for MMIO safety */
        for (size_t i = 0; i < len; i++) {
            d[i] = s[i];
        }
        __sync_synchronize();
    }
}

static inline void data_read(size_t offset, void *dst, size_t len)
{
    if (offset + len <= CXL_GPU_DATA_SIZE) {
        __sync_synchronize();
        uint8_t *d = (uint8_t *)dst;
        volatile uint8_t *s = g_data + offset;
        /* Use volatile access for MMIO safety */
        for (size_t i = 0; i < len; i++) {
            d[i] = s[i];
        }
    }
}

/* Execute command and wait for completion */
static CUresult execute_cmd(uint32_t cmd)
{
    reg_write32(CXL_GPU_REG_CMD, cmd);

    /* Wait for completion */
    int timeout = 1000000;
    while (timeout > 0) {
        uint32_t status = reg_read32(CXL_GPU_REG_CMD_STATUS);
        if (status == CXL_GPU_CMD_STATUS_COMPLETE) {
            return reg_read32(CXL_GPU_REG_CMD_RESULT);
        }
        if (status == CXL_GPU_CMD_STATUS_ERROR) {
            return reg_read32(CXL_GPU_REG_CMD_RESULT);
        }
        timeout--;
    }

    return CUDA_ERROR_UNKNOWN;
}

/* Find and map CXL Type 2 device */
static int find_and_map_device(void)
{
    DIR *dir;
    struct dirent *entry;
    char path[256];
    char buf[32];
    int fd;
    uint16_t vendor, device;

    dir = opendir("/sys/bus/pci/devices");
    if (!dir) {
        DLOG("Cannot open /sys/bus/pci/devices\n");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        /* Check vendor ID */
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", entry->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        if (read(fd, buf, sizeof(buf)) <= 0) {
            close(fd);
            continue;
        }
        close(fd);
        vendor = strtol(buf, NULL, 16);

        /* Check device ID */
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", entry->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        if (read(fd, buf, sizeof(buf)) <= 0) {
            close(fd);
            continue;
        }
        close(fd);
        device = strtol(buf, NULL, 16);

        if (vendor == CXL_TYPE2_VENDOR_ID && device == CXL_TYPE2_DEVICE_ID) {
            DLOG("Found CXL Type 2 device at %s\n", entry->d_name);

            /* Enable device */
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/enable", entry->d_name);
            fd = open(path, O_WRONLY);
            if (fd >= 0) {
                write(fd, "1", 1);
                close(fd);
            }

            /* Get BAR2 resource info */
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource", entry->d_name);
            FILE *fp = fopen(path, "r");
            if (fp) {
                uint64_t start, end, flags;
                /* Skip BAR0 and BAR1 */
                for (int i = 0; i < 2; i++) {
                    if (fscanf(fp, "0x%lx 0x%lx 0x%lx\n", &start, &end, &flags) != 3) {
                        break;
                    }
                }
                /* Read BAR2 */
                if (fscanf(fp, "0x%lx 0x%lx 0x%lx", &start, &end, &flags) == 3) {
                    g_bar_size = end - start + 1;
                    DLOG("BAR2: start=0x%lx end=0x%lx size=%zu\n", start, end, g_bar_size);
                }
                fclose(fp);
            }

            /* Map BAR2 */
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource2", entry->d_name);
            g_pci_fd = open(path, O_RDWR | O_SYNC);
            if (g_pci_fd < 0) {
                DLOG("Cannot open %s: %s\n", path, strerror(errno));
                closedir(dir);
                return -1;
            }

            if (g_bar_size == 0) {
                g_bar_size = CXL_GPU_CMD_REG_SIZE;
            }

            void *map = mmap(NULL, g_bar_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, g_pci_fd, 0);
            if (map == MAP_FAILED) {
                DLOG("Cannot mmap BAR2: %s\n", strerror(errno));
                close(g_pci_fd);
                g_pci_fd = -1;
                closedir(dir);
                return -1;
            }

            g_regs = (volatile uint32_t *)map;
            g_data = (volatile uint8_t *)map + CXL_GPU_DATA_OFFSET;

            /* Verify magic number */
            uint32_t magic = reg_read32(CXL_GPU_REG_MAGIC);
            if (magic != CXL_GPU_MAGIC) {
                DLOG("Invalid magic: 0x%x (expected 0x%x)\n", magic, CXL_GPU_MAGIC);
                munmap(map, g_bar_size);
                close(g_pci_fd);
                g_pci_fd = -1;
                g_regs = NULL;
                closedir(dir);
                return -1;
            }

            DLOG("Device mapped successfully, magic=0x%x version=0x%x\n",
                 magic, reg_read32(CXL_GPU_REG_VERSION));

            closedir(dir);
            return 0;
        }
    }

    closedir(dir);
    DLOG("CXL Type 2 device not found\n");
    return -1;
}

/* ========================================================================
 * CUDA Driver API Implementation
 * ======================================================================== */

CUresult cuInit(unsigned int flags)
{
    (void)flags;

    g_debug = (getenv("CXL_CUDA_DEBUG") != NULL);
    DLOG("cuInit(%u)\n", flags);

    if (g_initialized) {
        return CUDA_SUCCESS;
    }

    if (find_and_map_device() < 0) {
        return CUDA_ERROR_NO_DEVICE;
    }

    /* Check device status */
    uint32_t status = reg_read32(CXL_GPU_REG_STATUS);
    if (!(status & CXL_GPU_STATUS_READY)) {
        DLOG("Device not ready, status=0x%x\n", status);
        return CUDA_ERROR_NOT_INITIALIZED;
    }

    g_initialized = 1;
    DLOG("Initialization complete\n");
    return CUDA_SUCCESS;
}

CUresult cuDriverGetVersion(int *version)
{
    DLOG("cuDriverGetVersion\n");
    if (!version) return CUDA_ERROR_INVALID_VALUE;
    *version = 12000; /* CUDA 12.0 */
    return CUDA_SUCCESS;
}

CUresult cuDeviceGetCount(int *count)
{
    DLOG("cuDeviceGetCount\n");
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!count) return CUDA_ERROR_INVALID_VALUE;

    CUresult err = execute_cmd(CXL_GPU_CMD_GET_DEVICE_COUNT);
    if (err == CUDA_SUCCESS) {
        *count = reg_read64(CXL_GPU_REG_RESULT0);
        DLOG("  count=%d\n", *count);
    }
    return err;
}

CUresult cuDeviceGet(CUdevice *device, int ordinal)
{
    DLOG("cuDeviceGet(ordinal=%d)\n", ordinal);
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!device) return CUDA_ERROR_INVALID_VALUE;
    if (ordinal != 0) return CUDA_ERROR_INVALID_DEVICE;

    reg_write64(CXL_GPU_REG_PARAM0, ordinal);
    CUresult err = execute_cmd(CXL_GPU_CMD_GET_DEVICE);
    if (err == CUDA_SUCCESS) {
        *device = reg_read64(CXL_GPU_REG_RESULT0);
    }
    return err;
}

CUresult cuDeviceGetName(char *name, int len, CUdevice dev)
{
    DLOG("cuDeviceGetName(dev=%d)\n", dev);
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!name || len <= 0) return CUDA_ERROR_INVALID_VALUE;

    /* Read device name from registers */
    size_t to_read = (len < 64) ? len : 64;
    for (size_t i = 0; i < to_read; i += 8) {
        uint64_t val = reg_read64(CXL_GPU_REG_DEV_NAME + i);
        memcpy(name + i, &val, (to_read - i < 8) ? to_read - i : 8);
    }
    name[len - 1] = '\0';
    DLOG("  name=%s\n", name);
    return CUDA_SUCCESS;
}

CUresult cuDeviceTotalMem_v2(size_t *bytes, CUdevice dev)
{
    DLOG("cuDeviceTotalMem(dev=%d)\n", dev);
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!bytes) return CUDA_ERROR_INVALID_VALUE;

    *bytes = reg_read64(CXL_GPU_REG_TOTAL_MEM);
    DLOG("  bytes=%zu\n", *bytes);
    return CUDA_SUCCESS;
}

CUresult cuDeviceGetAttribute(int *value, int attrib, CUdevice dev)
{
    DLOG("cuDeviceGetAttribute(attrib=%d, dev=%d)\n", attrib, dev);
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!value) return CUDA_ERROR_INVALID_VALUE;

    switch (attrib) {
    case CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK:
        *value = reg_read32(CXL_GPU_REG_MAX_THREADS);
        break;
    case CU_DEVICE_ATTRIBUTE_WARP_SIZE:
        *value = reg_read32(CXL_GPU_REG_WARP_SIZE);
        break;
    case CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT:
        *value = reg_read32(CXL_GPU_REG_MP_COUNT);
        break;
    case CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR:
        *value = reg_read32(CXL_GPU_REG_CC_MAJOR);
        break;
    case CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR:
        *value = reg_read32(CXL_GPU_REG_CC_MINOR);
        break;
    case CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X:
    case CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Y:
        *value = 1024;
        break;
    case CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_Z:
        *value = 64;
        break;
    case CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X:
    case CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Y:
    case CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_Z:
        *value = 65535;
        break;
    default:
        *value = 0;
        break;
    }
    DLOG("  value=%d\n", *value);
    return CUDA_SUCCESS;
}

CUresult cuCtxCreate_v2(CUcontext *ctx, unsigned int flags, CUdevice dev)
{
    DLOG("cuCtxCreate(flags=%u, dev=%d)\n", flags, dev);
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!ctx) return CUDA_ERROR_INVALID_VALUE;

    CUresult err = execute_cmd(CXL_GPU_CMD_CTX_CREATE);
    if (err == CUDA_SUCCESS) {
        *ctx = (CUcontext)(uintptr_t)reg_read64(CXL_GPU_REG_RESULT0);
        g_context = *ctx;
        DLOG("  ctx=%p\n", *ctx);
    }
    return err;
}

CUresult cuCtxDestroy_v2(CUcontext ctx)
{
    DLOG("cuCtxDestroy(ctx=%p)\n", ctx);
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;

    CUresult err = execute_cmd(CXL_GPU_CMD_CTX_DESTROY);
    if (err == CUDA_SUCCESS) {
        g_context = NULL;
    }
    return err;
}

CUresult cuCtxSynchronize(void)
{
    DLOG("cuCtxSynchronize\n");
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    return execute_cmd(CXL_GPU_CMD_CTX_SYNC);
}

CUresult cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize)
{
    DLOG("cuMemAlloc(size=%zu)\n", bytesize);
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!dptr) return CUDA_ERROR_INVALID_VALUE;

    reg_write64(CXL_GPU_REG_PARAM0, bytesize);
    CUresult err = execute_cmd(CXL_GPU_CMD_MEM_ALLOC);
    if (err == CUDA_SUCCESS) {
        *dptr = reg_read64(CXL_GPU_REG_RESULT0);
        DLOG("  dptr=0x%lx\n", (unsigned long)*dptr);
    }
    return err;
}

CUresult cuMemFree_v2(CUdeviceptr dptr)
{
    DLOG("cuMemFree(dptr=0x%lx)\n", (unsigned long)dptr);
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;

    reg_write64(CXL_GPU_REG_PARAM0, dptr);
    return execute_cmd(CXL_GPU_CMD_MEM_FREE);
}

CUresult cuMemcpyHtoD_v2(CUdeviceptr dstDevice, const void *srcHost, size_t byteCount)
{
    DLOG("cuMemcpyHtoD(dst=0x%lx, size=%zu)\n", (unsigned long)dstDevice, byteCount);
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!srcHost) return CUDA_ERROR_INVALID_VALUE;

    /* Transfer in chunks that fit in data buffer */
    size_t offset = 0;
    while (offset < byteCount) {
        size_t chunk = byteCount - offset;
        if (chunk > CXL_GPU_DATA_SIZE) {
            chunk = CXL_GPU_DATA_SIZE;
        }

        data_write(0, (const uint8_t *)srcHost + offset, chunk);
        reg_write64(CXL_GPU_REG_PARAM0, dstDevice + offset);
        reg_write64(CXL_GPU_REG_PARAM1, chunk);

        CUresult err = execute_cmd(CXL_GPU_CMD_MEM_COPY_HTOD);
        if (err != CUDA_SUCCESS) {
            return err;
        }
        offset += chunk;
    }

    return CUDA_SUCCESS;
}

CUresult cuMemcpyDtoH_v2(void *dstHost, CUdeviceptr srcDevice, size_t byteCount)
{
    DLOG("cuMemcpyDtoH(src=0x%lx, size=%zu)\n", (unsigned long)srcDevice, byteCount);
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!dstHost) return CUDA_ERROR_INVALID_VALUE;

    /* Transfer in chunks */
    size_t offset = 0;
    while (offset < byteCount) {
        size_t chunk = byteCount - offset;
        if (chunk > CXL_GPU_DATA_SIZE) {
            chunk = CXL_GPU_DATA_SIZE;
        }

        reg_write64(CXL_GPU_REG_PARAM0, srcDevice + offset);
        reg_write64(CXL_GPU_REG_PARAM1, chunk);

        CUresult err = execute_cmd(CXL_GPU_CMD_MEM_COPY_DTOH);
        if (err != CUDA_SUCCESS) {
            return err;
        }

        data_read(0, (uint8_t *)dstHost + offset, chunk);
        offset += chunk;
    }

    return CUDA_SUCCESS;
}

CUresult cuModuleLoadData(CUmodule *module, const void *image)
{
    DLOG("cuModuleLoadData\n");
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!module || !image) return CUDA_ERROR_INVALID_VALUE;

    /* Copy PTX to data buffer */
    size_t len = strlen((const char *)image) + 1;
    if (len > CXL_GPU_DATA_SIZE) {
        return CUDA_ERROR_INVALID_VALUE;
    }

    data_write(0, image, len);
    CUresult err = execute_cmd(CXL_GPU_CMD_MODULE_LOAD_PTX);
    if (err == CUDA_SUCCESS) {
        *module = (CUmodule)(uintptr_t)reg_read64(CXL_GPU_REG_RESULT0);
        DLOG("  module=%p\n", *module);
    }
    return err;
}

CUresult cuModuleLoadDataEx(CUmodule *module, const void *image,
                            unsigned int numOptions, void *options, void **optionValues)
{
    (void)numOptions;
    (void)options;
    (void)optionValues;
    return cuModuleLoadData(module, image);
}

CUresult cuModuleGetFunction(CUfunction *hfunc, CUmodule hmod, const char *name)
{
    DLOG("cuModuleGetFunction(mod=%p, name=%s)\n", hmod, name);
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!hfunc || !name) return CUDA_ERROR_INVALID_VALUE;

    reg_write64(CXL_GPU_REG_PARAM0, (uintptr_t)hmod);

    size_t len = strlen(name) + 1;
    if (len > CXL_GPU_DATA_SIZE) {
        return CUDA_ERROR_INVALID_VALUE;
    }
    data_write(0, name, len);

    CUresult err = execute_cmd(CXL_GPU_CMD_FUNC_GET);
    if (err == CUDA_SUCCESS) {
        *hfunc = (CUfunction)(uintptr_t)reg_read64(CXL_GPU_REG_RESULT0);
        DLOG("  func=%p\n", *hfunc);
    }
    return err;
}

CUresult cuLaunchKernel(CUfunction f,
                        unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
                        unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
                        unsigned int sharedMemBytes, CUstream hStream,
                        void **kernelParams, void **extra)
{
    (void)hStream;
    (void)extra;

    DLOG("cuLaunchKernel(f=%p, grid=(%u,%u,%u), block=(%u,%u,%u), shared=%u)\n",
         f, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ, sharedMemBytes);

    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;

    reg_write64(CXL_GPU_REG_PARAM0, (uintptr_t)f);
    reg_write64(CXL_GPU_REG_PARAM1, ((uint64_t)gridDimY << 32) | gridDimX);
    reg_write64(CXL_GPU_REG_PARAM2, ((uint64_t)blockDimX << 32) | gridDimZ);
    reg_write64(CXL_GPU_REG_PARAM3, ((uint64_t)blockDimZ << 32) | blockDimY);

    /* Count args and copy to data buffer */
    uint32_t num_args = 0;
    if (kernelParams) {
        while (kernelParams[num_args]) num_args++;
        if (num_args * sizeof(void*) <= CXL_GPU_DATA_SIZE) {
            data_write(0, kernelParams, num_args * sizeof(void*));
        }
    }
    reg_write64(CXL_GPU_REG_PARAM4, ((uint64_t)num_args << 32) | sharedMemBytes);

    return execute_cmd(CXL_GPU_CMD_LAUNCH_KERNEL);
}

CUresult cuStreamCreate(CUstream *phStream, unsigned int Flags)
{
    DLOG("cuStreamCreate(flags=%u)\n", Flags);
    (void)Flags;
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!phStream) return CUDA_ERROR_INVALID_VALUE;
    *phStream = (CUstream)1; /* Dummy stream */
    return CUDA_SUCCESS;
}

CUresult cuStreamDestroy_v2(CUstream hStream)
{
    DLOG("cuStreamDestroy\n");
    (void)hStream;
    return CUDA_SUCCESS;
}

CUresult cuStreamSynchronize(CUstream hStream)
{
    DLOG("cuStreamSynchronize\n");
    (void)hStream;
    return cuCtxSynchronize();
}

CUresult cuMemGetInfo_v2(size_t *free, size_t *total)
{
    DLOG("cuMemGetInfo\n");
    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;

    size_t total_mem = reg_read64(CXL_GPU_REG_TOTAL_MEM);
    size_t free_mem = reg_read64(CXL_GPU_REG_FREE_MEM);

    /* Fallback: if free is 0, report total as free */
    if (free_mem == 0) {
        free_mem = total_mem;
    }

    if (total) *total = total_mem;
    if (free) *free = free_mem;

    return CUDA_SUCCESS;
}

/* Version compatibility aliases */
CUresult cuDeviceTotalMem(size_t *bytes, CUdevice dev)
{
    return cuDeviceTotalMem_v2(bytes, dev);
}

CUresult cuCtxCreate(CUcontext *ctx, unsigned int flags, CUdevice dev)
{
    return cuCtxCreate_v2(ctx, flags, dev);
}

CUresult cuCtxDestroy(CUcontext ctx)
{
    return cuCtxDestroy_v2(ctx);
}

CUresult cuMemAlloc(CUdeviceptr *dptr, size_t bytesize)
{
    return cuMemAlloc_v2(dptr, bytesize);
}

CUresult cuMemFree(CUdeviceptr dptr)
{
    return cuMemFree_v2(dptr);
}

CUresult cuMemcpyHtoD(CUdeviceptr dst, const void *src, size_t bytes)
{
    return cuMemcpyHtoD_v2(dst, src, bytes);
}

CUresult cuMemcpyDtoH(void *dst, CUdeviceptr src, size_t bytes)
{
    return cuMemcpyDtoH_v2(dst, src, bytes);
}

CUresult cuMemGetInfo(size_t *free, size_t *total)
{
    return cuMemGetInfo_v2(free, total);
}

CUresult cuStreamDestroy(CUstream hStream)
{
    return cuStreamDestroy_v2(hStream);
}

/* Additional API functions for comprehensive testing */

CUresult cuCtxGetCurrent(CUcontext *pctx)
{
    DLOG("cuCtxGetCurrent()\n");
    if (!pctx) return CUDA_ERROR_INVALID_VALUE;
    *pctx = g_context;
    return CUDA_SUCCESS;
}

CUresult cuCtxSetCurrent(CUcontext ctx)
{
    DLOG("cuCtxSetCurrent(%p)\n", ctx);
    g_context = ctx;
    return CUDA_SUCCESS;
}

CUresult cuCtxPushCurrent_v2(CUcontext ctx)
{
    DLOG("cuCtxPushCurrent_v2(%p)\n", ctx);
    g_context = ctx;
    return CUDA_SUCCESS;
}

CUresult cuCtxPopCurrent_v2(CUcontext *pctx)
{
    DLOG("cuCtxPopCurrent_v2()\n");
    if (pctx) *pctx = g_context;
    return CUDA_SUCCESS;
}

CUresult cuCtxGetDevice(CUdevice *device)
{
    DLOG("cuCtxGetDevice()\n");
    if (!device) return CUDA_ERROR_INVALID_VALUE;
    *device = 0;  /* Currently only support device 0 */
    return CUDA_SUCCESS;
}

CUresult cuMemcpyDtoD_v2(CUdeviceptr dstDevice, CUdeviceptr srcDevice, size_t byteCount)
{
    DLOG("cuMemcpyDtoD_v2(dst=0x%lx, src=0x%lx, size=%zu)\n",
         (unsigned long)dstDevice, (unsigned long)srcDevice, byteCount);

    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!dstDevice || !srcDevice) return CUDA_ERROR_INVALID_VALUE;

    /* For D2D copy, use intermediate host buffer via data region */
    size_t offset = 0;
    while (offset < byteCount) {
        size_t chunk = byteCount - offset;
        if (chunk > CXL_GPU_DATA_SIZE) {
            chunk = CXL_GPU_DATA_SIZE;
        }

        /* Read from source device memory to data region */
        reg_write64(CXL_GPU_REG_PARAM0, srcDevice + offset);
        reg_write64(CXL_GPU_REG_PARAM1, chunk);
        CUresult result = execute_cmd(CXL_GPU_CMD_MEM_COPY_DTOH);
        if (result != CUDA_SUCCESS) {
            return result;
        }

        /* Write from data region to destination device memory */
        reg_write64(CXL_GPU_REG_PARAM0, dstDevice + offset);
        reg_write64(CXL_GPU_REG_PARAM1, chunk);
        result = execute_cmd(CXL_GPU_CMD_MEM_COPY_HTOD);
        if (result != CUDA_SUCCESS) {
            return result;
        }

        offset += chunk;
    }

    return CUDA_SUCCESS;
}

CUresult cuMemsetD8_v2(CUdeviceptr dstDevice, unsigned char uc, size_t N)
{
    DLOG("cuMemsetD8_v2(dst=0x%lx, val=0x%02x, count=%zu)\n",
         (unsigned long)dstDevice, uc, N);

    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;

    /* Fill data region with value and copy in chunks */
    size_t chunk_size = N < CXL_GPU_DATA_SIZE ? N : CXL_GPU_DATA_SIZE;
    uint8_t *temp = (uint8_t *)malloc(chunk_size);
    if (!temp) return CUDA_ERROR_OUT_OF_MEMORY;
    memset(temp, uc, chunk_size);

    size_t offset = 0;
    while (offset < N) {
        size_t to_copy = (N - offset) < chunk_size ? (N - offset) : chunk_size;
        data_write(0, temp, to_copy);
        reg_write64(CXL_GPU_REG_PARAM0, dstDevice + offset);
        reg_write64(CXL_GPU_REG_PARAM1, to_copy);
        CUresult result = execute_cmd(CXL_GPU_CMD_MEM_COPY_HTOD);
        if (result != CUDA_SUCCESS) {
            free(temp);
            return result;
        }
        offset += to_copy;
    }

    free(temp);
    return CUDA_SUCCESS;
}

CUresult cuMemsetD32_v2(CUdeviceptr dstDevice, unsigned int ui, size_t N)
{
    DLOG("cuMemsetD32_v2(dst=0x%lx, val=0x%08x, count=%zu)\n",
         (unsigned long)dstDevice, ui, N);

    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;

    /* Fill data region with value and copy in chunks */
    size_t chunk_elements = CXL_GPU_DATA_SIZE / sizeof(unsigned int);
    size_t chunk_size = chunk_elements * sizeof(unsigned int);
    unsigned int *temp = (unsigned int *)malloc(chunk_size);
    if (!temp) return CUDA_ERROR_OUT_OF_MEMORY;
    for (size_t i = 0; i < chunk_elements; i++) {
        temp[i] = ui;
    }

    size_t elements_done = 0;
    while (elements_done < N) {
        size_t to_copy = (N - elements_done) < chunk_elements ? (N - elements_done) : chunk_elements;
        data_write(0, temp, to_copy * sizeof(unsigned int));
        reg_write64(CXL_GPU_REG_PARAM0, dstDevice + elements_done * sizeof(unsigned int));
        reg_write64(CXL_GPU_REG_PARAM1, to_copy * sizeof(unsigned int));
        CUresult result = execute_cmd(CXL_GPU_CMD_MEM_COPY_HTOD);
        if (result != CUDA_SUCCESS) {
            free(temp);
            return result;
        }
        elements_done += to_copy;
    }

    free(temp);
    return CUDA_SUCCESS;
}

CUresult cuMemGetAddressRange_v2(CUdeviceptr *pbase, size_t *psize, CUdeviceptr dptr)
{
    DLOG("cuMemGetAddressRange_v2(dptr=0x%lx)\n", (unsigned long)dptr);

    /* Return the pointer itself as base; size is unknown without tracking */
    if (pbase) *pbase = dptr;
    if (psize) *psize = 0;  /* Unknown */
    return CUDA_SUCCESS;
}

CUresult cuPointerGetAttribute(void *data, int attribute, CUdeviceptr ptr)
{
    DLOG("cuPointerGetAttribute(attr=%d, ptr=0x%lx)\n", attribute, (unsigned long)ptr);

    if (!data) return CUDA_ERROR_INVALID_VALUE;

    /* Minimal implementation */
    switch (attribute) {
        case 1:  /* CU_POINTER_ATTRIBUTE_CONTEXT */
            *(CUcontext *)data = g_context;
            return CUDA_SUCCESS;
        case 2:  /* CU_POINTER_ATTRIBUTE_MEMORY_TYPE */
            *(int *)data = 2;  /* Device memory */
            return CUDA_SUCCESS;
        default:
            return CUDA_ERROR_INVALID_VALUE;
    }
}

CUresult cuModuleUnload(CUmodule hmod)
{
    DLOG("cuModuleUnload(%p)\n", hmod);
    /* Modules are managed by hetGPU backend */
    return CUDA_SUCCESS;
}

CUresult cuEventCreate(CUevent *phEvent, unsigned int Flags)
{
    DLOG("cuEventCreate(flags=%u)\n", Flags);
    if (!phEvent) return CUDA_ERROR_INVALID_VALUE;
    /* Create a dummy event handle */
    static int event_counter = 0;
    *phEvent = (CUevent)(uintptr_t)(++event_counter);
    return CUDA_SUCCESS;
}

CUresult cuEventDestroy_v2(CUevent hEvent)
{
    DLOG("cuEventDestroy_v2(%p)\n", hEvent);
    return CUDA_SUCCESS;
}

CUresult cuEventRecord(CUevent hEvent, CUstream hStream)
{
    DLOG("cuEventRecord(%p, stream=%p)\n", hEvent, hStream);
    return CUDA_SUCCESS;
}

CUresult cuEventSynchronize(CUevent hEvent)
{
    DLOG("cuEventSynchronize(%p)\n", hEvent);
    return CUDA_SUCCESS;
}

CUresult cuEventElapsedTime(float *pMilliseconds, CUevent hStart, CUevent hEnd)
{
    DLOG("cuEventElapsedTime(%p, %p)\n", hStart, hEnd);
    if (!pMilliseconds) return CUDA_ERROR_INVALID_VALUE;
    /* Return a dummy elapsed time since we don't have real timing */
    *pMilliseconds = 0.001f;
    return CUDA_SUCCESS;
}

CUresult cuDeviceGetUuid(void *uuid, CUdevice dev)
{
    DLOG("cuDeviceGetUuid(dev=%d)\n", dev);
    if (!uuid) return CUDA_ERROR_INVALID_VALUE;
    /* Generate a deterministic UUID based on device number */
    memset(uuid, 0, 16);
    ((unsigned char *)uuid)[0] = 0xCE;  /* CXL */
    ((unsigned char *)uuid)[1] = 0x10;  /* Type 2 */
    ((unsigned char *)uuid)[15] = (unsigned char)dev;
    return CUDA_SUCCESS;
}

/* ============================================================================
 * P2P DMA Functions - Transfer data between GPU and CXL Type 3 memory
 * ============================================================================ */

/* P2P peer info structure */
typedef struct {
    uint32_t peer_id;
    uint32_t peer_type;  /* CXL_P2P_PEER_TYPE2 or CXL_P2P_PEER_TYPE3 */
    uint64_t mem_size;
    int coherent;
} CXLPeerInfo;

/* Discover P2P peer devices on the CXL fabric */
int cxl_p2p_discover_peers(int *num_peers)
{
    DLOG("cxl_p2p_discover_peers()\n");

    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;

    CUresult result = execute_cmd(CXL_GPU_CMD_P2P_DISCOVER);
    if (result != CUDA_SUCCESS) {
        return result;
    }

    if (num_peers) {
        *num_peers = (int)reg_read64(CXL_GPU_REG_RESULT0);
    }

    DLOG("  discovered %d peers\n", num_peers ? *num_peers : -1);
    return CUDA_SUCCESS;
}

/* Get peer device information */
int cxl_p2p_get_peer_info(uint32_t peer_id, CXLPeerInfo *info)
{
    DLOG("cxl_p2p_get_peer_info(peer=%u)\n", peer_id);

    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (!info) return CUDA_ERROR_INVALID_VALUE;

    reg_write64(CXL_GPU_REG_PARAM0, peer_id);
    CUresult result = execute_cmd(CXL_GPU_CMD_P2P_GET_PEER_INFO);
    if (result != CUDA_SUCCESS) {
        return result;
    }

    info->peer_id = peer_id;
    info->peer_type = (uint32_t)reg_read64(CXL_GPU_REG_RESULT0);
    info->mem_size = reg_read64(CXL_GPU_REG_RESULT1);
    info->coherent = (int)reg_read64(CXL_GPU_REG_RESULT2);

    DLOG("  peer %u: type=%u, size=%lu MB, coherent=%d\n",
         peer_id, info->peer_type, info->mem_size / (1024*1024), info->coherent);
    return CUDA_SUCCESS;
}

/* Transfer data from GPU memory to Type 3 CXL memory */
int cxl_p2p_gpu_to_mem(uint32_t t3_peer_id, uint64_t gpu_offset,
                       uint64_t mem_offset, uint64_t size)
{
    DLOG("cxl_p2p_gpu_to_mem(peer=%u, gpu_off=0x%lx, mem_off=0x%lx, size=%lu)\n",
         t3_peer_id, (unsigned long)gpu_offset, (unsigned long)mem_offset,
         (unsigned long)size);

    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (size == 0) return CUDA_SUCCESS;

    reg_write64(CXL_GPU_REG_PARAM0, t3_peer_id);
    reg_write64(CXL_GPU_REG_PARAM1, gpu_offset);
    reg_write64(CXL_GPU_REG_PARAM2, mem_offset);
    reg_write64(CXL_GPU_REG_PARAM3, size);

    CUresult result = execute_cmd(CXL_GPU_CMD_P2P_GPU_TO_MEM);
    if (result != CUDA_SUCCESS) {
        DLOG("  P2P GPU->MEM transfer failed: %d\n", result);
    }
    return result;
}

/* Transfer data from Type 3 CXL memory to GPU memory */
int cxl_p2p_mem_to_gpu(uint32_t t3_peer_id, uint64_t mem_offset,
                       uint64_t gpu_offset, uint64_t size)
{
    DLOG("cxl_p2p_mem_to_gpu(peer=%u, mem_off=0x%lx, gpu_off=0x%lx, size=%lu)\n",
         t3_peer_id, (unsigned long)mem_offset, (unsigned long)gpu_offset,
         (unsigned long)size);

    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (size == 0) return CUDA_SUCCESS;

    reg_write64(CXL_GPU_REG_PARAM0, t3_peer_id);
    reg_write64(CXL_GPU_REG_PARAM1, mem_offset);
    reg_write64(CXL_GPU_REG_PARAM2, gpu_offset);
    reg_write64(CXL_GPU_REG_PARAM3, size);

    CUresult result = execute_cmd(CXL_GPU_CMD_P2P_MEM_TO_GPU);
    if (result != CUDA_SUCCESS) {
        DLOG("  P2P MEM->GPU transfer failed: %d\n", result);
    }
    return result;
}

/* Transfer data between two Type 3 CXL memory devices */
int cxl_p2p_mem_to_mem(uint32_t src_peer_id, uint32_t dst_peer_id,
                       uint64_t src_offset, uint64_t dst_offset, uint64_t size)
{
    DLOG("cxl_p2p_mem_to_mem(src=%u, dst=%u, src_off=0x%lx, dst_off=0x%lx, size=%lu)\n",
         src_peer_id, dst_peer_id, (unsigned long)src_offset,
         (unsigned long)dst_offset, (unsigned long)size);

    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;
    if (size == 0) return CUDA_SUCCESS;

    reg_write64(CXL_GPU_REG_PARAM0, src_peer_id);
    reg_write64(CXL_GPU_REG_PARAM1, dst_peer_id);
    reg_write64(CXL_GPU_REG_PARAM2, src_offset);
    reg_write64(CXL_GPU_REG_PARAM3, dst_offset);
    reg_write64(CXL_GPU_REG_PARAM4, size);

    CUresult result = execute_cmd(CXL_GPU_CMD_P2P_MEM_TO_MEM);
    if (result != CUDA_SUCCESS) {
        DLOG("  P2P MEM->MEM transfer failed: %d\n", result);
    }
    return result;
}

/* Wait for all pending P2P transfers to complete */
int cxl_p2p_sync(void)
{
    DLOG("cxl_p2p_sync()\n");

    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;

    return execute_cmd(CXL_GPU_CMD_P2P_SYNC);
}

/* Get P2P engine status and statistics */
int cxl_p2p_get_status(int *num_peers, uint64_t *transfers_completed,
                       uint64_t *bytes_transferred)
{
    DLOG("cxl_p2p_get_status()\n");

    if (!g_initialized) return CUDA_ERROR_NOT_INITIALIZED;

    CUresult result = execute_cmd(CXL_GPU_CMD_P2P_GET_STATUS);
    if (result != CUDA_SUCCESS) {
        return result;
    }

    if (num_peers) {
        *num_peers = (int)reg_read64(CXL_GPU_REG_RESULT0);
    }
    if (transfers_completed) {
        *transfers_completed = reg_read64(CXL_GPU_REG_RESULT1);
    }
    if (bytes_transferred) {
        *bytes_transferred = reg_read64(CXL_GPU_REG_RESULT2);
    }

    return CUDA_SUCCESS;
}

/* Library initialization/cleanup */
__attribute__((constructor))
static void libcuda_init(void)
{
    DLOG("libcuda.so loaded (CXL Type 2 shim)\n");
}

__attribute__((destructor))
static void libcuda_cleanup(void)
{
    DLOG("libcuda.so unloading\n");
    if (g_regs) {
        munmap((void *)g_regs, g_bar_size);
        g_regs = NULL;
    }
    if (g_pci_fd >= 0) {
        close(g_pci_fd);
        g_pci_fd = -1;
    }
    g_initialized = 0;
}
