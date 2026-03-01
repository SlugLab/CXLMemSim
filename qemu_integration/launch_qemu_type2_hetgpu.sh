#!/bin/bash

# Launch QEMU with CXL Type 2 Device using hetGPU Backend
# This script demonstrates using the CXL Type 2 device with hetGPU, which provides:
# - CUDA compatibility on any GPU (Intel, AMD, etc.)
# - PTX translation to native GPU code
# - Cache coherency (Type 1 feature)
# - Device-attached memory (Type 3 feature)
# - CPU-GPU coherency through CXL.cache protocol

set -e

# Default paths
QEMU_BINARY=${QEMU_BINARY:-/usr/local/bin/qemu-system-x86_64}
HETGPU_PATH=${HETGPU_PATH:-/home/victoryang00/hetGPU}

# Prefer real NVIDIA libcuda.so if available (for direct H100/A100 access)

HETGPU_LIB="${HETGPU_PATH}/target/debug/libnvcuda.so"


# CXLMemSim configuration
CXL_MEMSIM_HOST=${CXL_MEMSIM_HOST:-127.0.0.1}
CXL_MEMSIM_PORT=${CXL_MEMSIM_PORT:-9999}

# VM configuration
VM_MEMORY=${VM_MEMORY:-16G}
DISK_IMAGE=${DISK_IMAGE:-qemu1.img}
KERNEL_IMAGE=${KERNEL_IMAGE:-/home/victoryang00/cxl/arch/x86/boot/bzImage}
INITRD_IMAGE=${INITRD_IMAGE:-/boot/initrd.img-6.18.0-rc5}

# Type 2 device configuration
CXL_TYPE2_CACHE_SIZE=${CXL_TYPE2_CACHE_SIZE:-128M}
CXL_TYPE2_MEM_SIZE=${CXL_TYPE2_MEM_SIZE:-4G}

# hetGPU configuration
# Backend types: 0=auto, 1=intel, 2=amd, 3=nvidia, 4=tenstorrent, 5=simulation
HETGPU_BACKEND=${HETGPU_BACKEND:-3}

# Device indices for dual-GPU (MIG or multi-GPU)
# With MIG: each index maps to a separate MIG instance on one physical GPU.
# Without MIG: both can use device 0 (context-switched via QEMU mutex).
# To set up MIG on H100/A100:
#   sudo nvidia-smi -i 0 -mig 1
#   sudo nvidia-smi mig -cgi 9,9 -C    # Create 2 equal MIG instances
HETGPU_DEVICE0=${HETGPU_DEVICE0:-0}
HETGPU_DEVICE1=${HETGPU_DEVICE1:-1}

# GPU mode: 0=none, 1=vfio, 2=hetgpu, 3=auto
GPU_MODE=${GPU_MODE:-2}

# Transport mode: tcp or shm (shared memory)
export CXL_TRANSPORT_MODE=${CXL_TRANSPORT_MODE:-shm}
export CXL_MEMSIM_HOST=$CXL_MEMSIM_HOST
export CXL_MEMSIM_PORT=$CXL_MEMSIM_PORT
export HETGPU_LIB_PATH=$HETGPU_LIB

# Check if GPU library exists
if [ ! -f "$HETGPU_LIB" ]; then
    echo "Warning: GPU library not found at $HETGPU_LIB"
    echo "Will use simulation mode. To use real GPU:"
    echo "  For NVIDIA: Install CUDA driver (libcuda.so)"
    echo "  For other GPUs: Build hetGPU: cd $HETGPU_PATH && cargo build --release"
fi

# Display which library will be used
if [[ "$HETGPU_LIB" == *"libnvcuda"* ]]; then
    echo "Using hetGPU library (CUDA translation for non-NVIDIA GPUs)"
else
    echo "Using real NVIDIA libcuda.so (direct H100/A100 access)"
fi

echo "==================================================================="
echo "CXL Type 2 Device with hetGPU Backend Configuration"
echo "==================================================================="
echo "QEMU Binary: $QEMU_BINARY"
echo "hetGPU Library: $HETGPU_LIB"
echo "hetGPU Backend: $HETGPU_BACKEND (0=auto, 1=intel, 2=amd, 3=nvidia, 4=tenstorrent, 5=sim)"
echo "hetGPU Device 0: $HETGPU_DEVICE0"
echo "hetGPU Device 1: $HETGPU_DEVICE1"
echo "GPU Mode: $GPU_MODE (0=none, 1=vfio, 2=hetgpu, 3=auto)"
echo "-------------------------------------------------------------------"
echo "CXLMemSim Server: $CXL_MEMSIM_HOST:$CXL_MEMSIM_PORT"
echo "Transport Mode: $CXL_TRANSPORT_MODE"
echo "Type 2 Cache Size: $CXL_TYPE2_CACHE_SIZE"
echo "Type 2 Device Memory: $CXL_TYPE2_MEM_SIZE"
echo "==================================================================="

# Construct the QEMU command
exec $QEMU_BINARY \
    --enable-kvm -cpu qemu64,+xsave,+rdtscp,+avx,+avx2,+sse4.1,+sse4.2,+avx512f,+avx512dq,+avx512ifma,+avx512cd,+avx512bw,+avx512vl,+avx512vbmi,+clflushopt  \
    -kernel $KERNEL_IMAGE \
    -append "root=/dev/vda rw console=ttyS0,115200 nokaslr" \
    -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:00:00:01 \
    -device virtio-rng-pci \
    -drive file=./$DISK_IMAGE,if=none,id=disk0,format=raw \
    -device virtio-blk-pci,drive=disk0,bus=pcie.0 \
    -M q35,cxl=on \
    -m ${VM_MEMORY},maxmem=32G,slots=8 \
    -smp 4 \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=0 \
    -device cxl-rp,port=1,bus=cxl.1,id=root_port14,chassis=0,slot=1 \
    -device cxl-type2,bus=root_port13,\
cache-size=$CXL_TYPE2_CACHE_SIZE,\
mem-size=$CXL_TYPE2_MEM_SIZE,\
sn=0x2,\
cxlmemsim-addr=$CXL_MEMSIM_HOST,\
cxlmemsim-port=$CXL_MEMSIM_PORT,\
coherency-enabled=true,\
gpu-mode=$GPU_MODE,\
hetgpu-lib=$HETGPU_LIB,\
hetgpu-device=$HETGPU_DEVICE0,\
hetgpu-backend=$HETGPU_BACKEND,\
id=cxl-type2-hetgpu0 \
    -device cxl-type2,bus=root_port14,\
cache-size=$CXL_TYPE2_CACHE_SIZE,\
mem-size=$CXL_TYPE2_MEM_SIZE,\
sn=0x3,\
cxlmemsim-addr=$CXL_MEMSIM_HOST,\
cxlmemsim-port=$CXL_MEMSIM_PORT,\
coherency-enabled=true,\
gpu-mode=$GPU_MODE,\
hetgpu-lib=$HETGPU_LIB,\
hetgpu-device=$HETGPU_DEVICE1,\
hetgpu-backend=$HETGPU_BACKEND,\
id=cxl-type2-hetgpu1 \
    -nographic
