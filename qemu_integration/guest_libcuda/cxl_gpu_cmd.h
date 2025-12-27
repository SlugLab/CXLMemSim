/*
 * CXL Type 2 GPU Command Interface - Guest Header
 * Shared between host QEMU and guest libcuda shim
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef CXL_GPU_CMD_H
#define CXL_GPU_CMD_H

#include <stdint.h>

/* GPU Command Register Offsets (from BAR2 base) */
#define CXL_GPU_REG_MAGIC           0x0000  /* Magic number: 0x43584C32 "CXL2" */
#define CXL_GPU_REG_VERSION         0x0004  /* Interface version */
#define CXL_GPU_REG_STATUS          0x0008  /* Device status */
#define CXL_GPU_REG_CAPS            0x000C  /* Device capabilities */

#define CXL_GPU_REG_CMD             0x0010  /* Command register */
#define CXL_GPU_REG_CMD_STATUS      0x0014  /* Command status */
#define CXL_GPU_REG_CMD_RESULT      0x0018  /* Command result/error code */
#define CXL_GPU_REG_CMD_DATA_LO     0x001C  /* Command data low 32 bits */
#define CXL_GPU_REG_CMD_DATA_HI     0x0020  /* Command data high 32 bits */

#define CXL_GPU_REG_PARAM0          0x0040  /* Parameter 0 */
#define CXL_GPU_REG_PARAM1          0x0048  /* Parameter 1 */
#define CXL_GPU_REG_PARAM2          0x0050  /* Parameter 2 */
#define CXL_GPU_REG_PARAM3          0x0058  /* Parameter 3 */
#define CXL_GPU_REG_PARAM4          0x0060  /* Parameter 4 */
#define CXL_GPU_REG_PARAM5          0x0068  /* Parameter 5 */
#define CXL_GPU_REG_PARAM6          0x0070  /* Parameter 6 */
#define CXL_GPU_REG_PARAM7          0x0078  /* Parameter 7 */

#define CXL_GPU_REG_RESULT0         0x0080  /* Result 0 */
#define CXL_GPU_REG_RESULT1         0x0088  /* Result 1 */
#define CXL_GPU_REG_RESULT2         0x0090  /* Result 2 */
#define CXL_GPU_REG_RESULT3         0x0098  /* Result 3 */

/* Device info registers */
#define CXL_GPU_REG_DEV_NAME        0x0100  /* Device name (64 bytes) */
#define CXL_GPU_REG_TOTAL_MEM       0x0140  /* Total memory */
#define CXL_GPU_REG_FREE_MEM        0x0148  /* Free memory */
#define CXL_GPU_REG_CC_MAJOR        0x0150  /* Compute capability major */
#define CXL_GPU_REG_CC_MINOR        0x0154  /* Compute capability minor */
#define CXL_GPU_REG_MP_COUNT        0x0158  /* Multiprocessor count */
#define CXL_GPU_REG_MAX_THREADS     0x015C  /* Max threads per block */
#define CXL_GPU_REG_WARP_SIZE       0x0160  /* Warp size */
#define CXL_GPU_REG_BACKEND         0x0164  /* Backend type */

/* Data transfer region */
#define CXL_GPU_DATA_OFFSET         0x1000  /* Data buffer offset */
#define CXL_GPU_DATA_SIZE           0xF000  /* Data buffer size (60KB) */

/* Command register size */
#define CXL_GPU_CMD_REG_SIZE        0x10000 /* 64KB total */

/* Magic number */
#define CXL_GPU_MAGIC               0x43584C32  /* "CXL2" */
#define CXL_GPU_VERSION             0x00010000  /* v1.0.0 */

/* Device status bits */
#define CXL_GPU_STATUS_READY        (1 << 0)
#define CXL_GPU_STATUS_BUSY         (1 << 1)
#define CXL_GPU_STATUS_ERROR        (1 << 2)
#define CXL_GPU_STATUS_CTX_ACTIVE   (1 << 3)

/* Command status */
#define CXL_GPU_CMD_STATUS_IDLE     0
#define CXL_GPU_CMD_STATUS_PENDING  1
#define CXL_GPU_CMD_STATUS_RUNNING  2
#define CXL_GPU_CMD_STATUS_COMPLETE 3
#define CXL_GPU_CMD_STATUS_ERROR    4

/* GPU Commands */
typedef enum {
    CXL_GPU_CMD_NOP             = 0x00,
    CXL_GPU_CMD_INIT            = 0x01,
    CXL_GPU_CMD_GET_DEVICE_COUNT= 0x02,
    CXL_GPU_CMD_GET_DEVICE      = 0x03,
    CXL_GPU_CMD_GET_DEVICE_NAME = 0x04,
    CXL_GPU_CMD_GET_DEVICE_PROPS= 0x05,
    CXL_GPU_CMD_GET_TOTAL_MEM   = 0x06,

    CXL_GPU_CMD_CTX_CREATE      = 0x10,
    CXL_GPU_CMD_CTX_DESTROY     = 0x11,
    CXL_GPU_CMD_CTX_SYNC        = 0x12,

    CXL_GPU_CMD_MEM_ALLOC       = 0x20,
    CXL_GPU_CMD_MEM_FREE        = 0x21,
    CXL_GPU_CMD_MEM_COPY_HTOD   = 0x22,
    CXL_GPU_CMD_MEM_COPY_DTOH   = 0x23,
    CXL_GPU_CMD_MEM_COPY_DTOD   = 0x24,
    CXL_GPU_CMD_MEM_SET         = 0x25,
    CXL_GPU_CMD_MEM_GET_INFO    = 0x26,

    CXL_GPU_CMD_MODULE_LOAD_PTX = 0x30,
    CXL_GPU_CMD_MODULE_UNLOAD   = 0x31,
    CXL_GPU_CMD_FUNC_GET        = 0x32,

    CXL_GPU_CMD_LAUNCH_KERNEL   = 0x40,

    CXL_GPU_CMD_STREAM_CREATE   = 0x50,
    CXL_GPU_CMD_STREAM_DESTROY  = 0x51,
    CXL_GPU_CMD_STREAM_SYNC     = 0x52,

    CXL_GPU_CMD_EVENT_CREATE    = 0x60,
    CXL_GPU_CMD_EVENT_DESTROY   = 0x61,
    CXL_GPU_CMD_EVENT_RECORD    = 0x62,
    CXL_GPU_CMD_EVENT_SYNC      = 0x63,
} CXLGPUCommand;

/* Error codes (matching CUDA error codes) */
typedef enum {
    CXL_GPU_SUCCESS                     = 0,
    CXL_GPU_ERROR_INVALID_VALUE         = 1,
    CXL_GPU_ERROR_OUT_OF_MEMORY         = 2,
    CXL_GPU_ERROR_NOT_INITIALIZED       = 3,
    CXL_GPU_ERROR_DEINITIALIZED         = 4,
    CXL_GPU_ERROR_NO_DEVICE             = 100,
    CXL_GPU_ERROR_INVALID_DEVICE        = 101,
    CXL_GPU_ERROR_INVALID_CONTEXT       = 201,
    CXL_GPU_ERROR_INVALID_HANDLE        = 400,
    CXL_GPU_ERROR_NOT_FOUND             = 500,
    CXL_GPU_ERROR_NOT_READY             = 600,
    CXL_GPU_ERROR_LAUNCH_FAILED         = 700,
    CXL_GPU_ERROR_INVALID_PTX           = 800,
    CXL_GPU_ERROR_UNKNOWN               = 999,
} CXLGPUError;

#endif /* CXL_GPU_CMD_H */
