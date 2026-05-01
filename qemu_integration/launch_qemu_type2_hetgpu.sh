#!/bin/bash

# Launch QEMU with CXL Type 2 Device using hetGPU Backend
# This script demonstrates using the CXL Type 2 device with hetGPU, which provides:
# - CUDA compatibility on any GPU (Intel, AMD, etc.)
# - PTX translation to native GPU code
# - Cache coherency (Type 1 feature)
# - Device-attached memory (Type 3 feature)
# - CPU-GPU coherency through CXL.cache protocol

set -e

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD_DIR_DEFAULT="$REPO_ROOT/build"

# Default paths
if [ -x "$REPO_ROOT/lib/qemu/build/qemu-system-x86_64" ]; then
    QEMU_BINARY_DEFAULT="$REPO_ROOT/lib/qemu/build/qemu-system-x86_64"
else
    QEMU_BINARY_DEFAULT="/usr/local/bin/qemu-system-x86_64"
fi
QEMU_BINARY=${QEMU_BINARY:-$QEMU_BINARY_DEFAULT}
HETGPU_PATH=${HETGPU_PATH:-/home/victoryang00/hetGPU}

# Prefer real NVIDIA libcuda.so if available (for direct H100/A100 access)

HETGPU_LIB="${HETGPU_PATH}/target/debug/libnvcuda.so"


# CXLMemSim configuration
CXL_MEMSIM_HOST=${CXL_MEMSIM_HOST:-127.0.0.1}
CXL_MEMSIM_PORT=${CXL_MEMSIM_PORT:-9999}

# VM configuration
VM_MEMORY=${VM_MEMORY:-16G}
DISK_IMAGE=${DISK_IMAGE:-$BUILD_DIR_DEFAULT/qemu1.img}
KERNEL_IMAGE=${KERNEL_IMAGE:-/home/victoryang00/cxl/arch/x86/boot/bzImage}
INITRD_IMAGE=${INITRD_IMAGE:-/boot/initrd.img-6.18.0-rc5}
NET_MODE=${NET_MODE:-tap}
TAP_IFACE=${TAP_IFACE:-tap1}

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
HETGPU_DEVICE1=${HETGPU_DEVICE1:-0}

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
echo "Disk Image: $DISK_IMAGE"
echo "Kernel Image: $KERNEL_IMAGE"
echo "Network Mode: $NET_MODE"
echo "==================================================================="

if [ ! -x "$QEMU_BINARY" ]; then
    echo "ERROR: QEMU binary not found or not executable: $QEMU_BINARY" >&2
    exit 1
fi

if [ ! -f "$DISK_IMAGE" ]; then
    echo "ERROR: disk image not found: $DISK_IMAGE" >&2
    exit 1
fi

if [ ! -f "$KERNEL_IMAGE" ]; then
    echo "ERROR: kernel image not found: $KERNEL_IMAGE" >&2
    exit 1
fi

if command -v ldd >/dev/null 2>&1; then
    if ldd "$QEMU_BINARY" 2>/dev/null | grep -q "not found"; then
        echo "ERROR: QEMU runtime libraries are missing for $QEMU_BINARY" >&2
        ldd "$QEMU_BINARY" 2>/dev/null | grep "not found" >&2 || true
        exit 1
    fi
fi

if [ "$NET_MODE" = "tap" ]; then
    if ! ip link show "$TAP_IFACE" >/dev/null 2>&1; then
        echo "ERROR: tap interface $TAP_IFACE not found. Set NET_MODE=user or create $TAP_IFACE." >&2
        exit 1
    fi
    NET_ARGS=(
        -netdev "tap,id=net0,ifname=$TAP_IFACE,script=no,downscript=no"
        -device virtio-net-pci,netdev=net0,mac=52:54:00:00:00:02
    )
else
    NET_ARGS=(
        -netdev user,id=net0,hostfwd=tcp::10022-:22
        -device virtio-net-pci,netdev=net0,mac=52:54:00:00:00:02
    )
fi

# Construct the QEMU command
exec "$QEMU_BINARY" \
    --enable-kvm -cpu qemu64,+xsave,+rdtscp,+avx,+avx2,+sse4.1,+sse4.2,+avx512f,+avx512dq,+avx512ifma,+avx512cd,+avx512bw,+avx512vl,+avx512vbmi,+clflushopt  \
    -kernel $KERNEL_IMAGE \
    -append "root=/dev/vda rw console=ttyS0,115200 nokaslr" \
    "${NET_ARGS[@]}" \
    -device virtio-rng-pci \
    -drive file=$DISK_IMAGE,if=none,id=disk0,format=raw \
    -device virtio-blk-pci,drive=disk0,bus=pcie.0 \
    -M q35,cxl=on \
    -m ${VM_MEMORY},maxmem=32G,slots=8 \
    -smp 4 \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=0 \
    -device cxl-rp,port=1,bus=cxl.1,id=root_port14,chassis=0,slot=1 \
    -device cxl-rp,port=2,bus=cxl.1,id=root_port15,chassis=0,slot=2 \
    -device cxl-type3,bus=root_port15,persistent-memdev=cxl-mem1,lsa=cxl-lsa1,id=cxl-pmem0,sn=0x1 \
    -object memory-backend-file,id=cxl-mem1,share=on,mem-path=/dev/shm/cxlmemsim_dist_node0,size=256M \
    -object memory-backend-file,id=cxl-lsa1,share=on,mem-path=/dev/shm/lsa0.raw,size=256K \
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
