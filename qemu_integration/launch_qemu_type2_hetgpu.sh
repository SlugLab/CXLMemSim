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
HETGPU_LIB=${HETGPU_LIB:-${HETGPU_PATH}/target/release/libnvcuda.so}

# CXLMemSim configuration
CXL_MEMSIM_HOST=${CXL_MEMSIM_HOST:-127.0.0.1}
CXL_MEMSIM_PORT=${CXL_MEMSIM_PORT:-9999}

# VM configuration
VM_MEMORY=${VM_MEMORY:-16G}
DISK_IMAGE=${DISK_IMAGE:-qemu1.img}
KERNEL_IMAGE=${KERNEL_IMAGE:-/home/victoryang00/cxl/arch/x86/boot/bzImage}
INITRD_IMAGE=${INITRD_IMAGE:-/boot/initrd.img-6.18.0-rc5+}

# Type 2 device configuration
CXL_TYPE2_CACHE_SIZE=${CXL_TYPE2_CACHE_SIZE:-128M}
CXL_TYPE2_MEM_SIZE=${CXL_TYPE2_MEM_SIZE:-4G}

# hetGPU configuration
# Backend types: 0=auto, 1=intel, 2=amd, 3=nvidia, 4=tenstorrent, 5=simulation
HETGPU_BACKEND=${HETGPU_BACKEND:-0}
HETGPU_DEVICE=${HETGPU_DEVICE:-0}

# GPU mode: 0=none, 1=vfio, 2=hetgpu, 3=auto
GPU_MODE=${GPU_MODE:-2}

# Transport mode: tcp or shm (shared memory)
export CXL_TRANSPORT_MODE=${CXL_TRANSPORT_MODE:-shm}
export CXL_MEMSIM_HOST=$CXL_MEMSIM_HOST
export CXL_MEMSIM_PORT=$CXL_MEMSIM_PORT
export HETGPU_LIB_PATH=$HETGPU_LIB

# Check if hetGPU library exists
if [ ! -f "$HETGPU_LIB" ]; then
    echo "Warning: hetGPU library not found at $HETGPU_LIB"
    echo "Will use simulation mode. To use real GPU:"
    echo "  1. Build hetGPU: cd $HETGPU_PATH && cargo build --release"
    echo "  2. Set HETGPU_LIB to the correct path"
fi

echo "==================================================================="
echo "CXL Type 2 Device with hetGPU Backend Configuration"
echo "==================================================================="
echo "QEMU Binary: $QEMU_BINARY"
echo "hetGPU Library: $HETGPU_LIB"
echo "hetGPU Backend: $HETGPU_BACKEND (0=auto, 1=intel, 2=amd, 3=nvidia, 4=tenstorrent, 5=sim)"
echo "hetGPU Device Index: $HETGPU_DEVICE"
echo "GPU Mode: $GPU_MODE (0=none, 1=vfio, 2=hetgpu, 3=auto)"
echo "-------------------------------------------------------------------"
echo "CXLMemSim Server: $CXL_MEMSIM_HOST:$CXL_MEMSIM_PORT"
echo "Transport Mode: $CXL_TRANSPORT_MODE"
echo "Type 2 Cache Size: $CXL_TYPE2_CACHE_SIZE"
echo "Type 2 Device Memory: $CXL_TYPE2_MEM_SIZE"
echo "==================================================================="

# Function to display usage
usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  --qemu PATH        Path to QEMU binary"
    echo "  --hetgpu-lib PATH  Path to hetGPU library (libnvcuda.so)"
    echo "  --hetgpu-path PATH Path to hetGPU root directory"
    echo "  --backend TYPE     hetGPU backend (auto|intel|amd|nvidia|tenstorrent|sim)"
    echo "  --device INDEX     GPU device index to use"
    echo "  --disk PATH        Path to disk image"
    echo "  --kernel PATH      Path to kernel image"
    echo "  --initrd PATH      Path to initrd/initramfs image"
    echo "  --memory SIZE      VM memory size (e.g., 16G)"
    echo "  --cache-size SIZE  CXL cache size (e.g., 128M)"
    echo "  --mem-size SIZE    CXL device memory size (e.g., 4G)"
    echo "  --memsim-host HOST CXLMemSim server host"
    echo "  --memsim-port PORT CXLMemSim server port"
    echo "  --help             Show this help message"
    exit 1
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --qemu)
            QEMU_BINARY="$2"
            shift 2
            ;;
        --hetgpu-lib)
            HETGPU_LIB="$2"
            export HETGPU_LIB_PATH="$2"
            shift 2
            ;;
        --hetgpu-path)
            HETGPU_PATH="$2"
            HETGPU_LIB="${HETGPU_PATH}/target/release/libnvcuda.so"
            export HETGPU_LIB_PATH="$HETGPU_LIB"
            shift 2
            ;;
        --backend)
            case $2 in
                auto) HETGPU_BACKEND=0 ;;
                intel) HETGPU_BACKEND=1 ;;
                amd) HETGPU_BACKEND=2 ;;
                nvidia) HETGPU_BACKEND=3 ;;
                tenstorrent) HETGPU_BACKEND=4 ;;
                sim|simulation) HETGPU_BACKEND=5 ;;
                *) echo "Unknown backend: $2"; usage ;;
            esac
            shift 2
            ;;
        --device)
            HETGPU_DEVICE="$2"
            shift 2
            ;;
        --disk)
            DISK_IMAGE="$2"
            shift 2
            ;;
        --kernel)
            KERNEL_IMAGE="$2"
            shift 2
            ;;
        --initrd)
            INITRD_IMAGE="$2"
            shift 2
            ;;
        --memory)
            VM_MEMORY="$2"
            shift 2
            ;;
        --cache-size)
            CXL_TYPE2_CACHE_SIZE="$2"
            shift 2
            ;;
        --mem-size)
            CXL_TYPE2_MEM_SIZE="$2"
            shift 2
            ;;
        --memsim-host)
            CXL_MEMSIM_HOST="$2"
            export CXL_MEMSIM_HOST="$2"
            shift 2
            ;;
        --memsim-port)
            CXL_MEMSIM_PORT="$2"
            export CXL_MEMSIM_PORT="$2"
            shift 2
            ;;
        --help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

# Construct the QEMU command
exec $QEMU_BINARY \
    --enable-kvm \
    -cpu host \
    -kernel $KERNEL_IMAGE \
    -initrd $INITRD_IMAGE \
    -append "root=/dev/vda rw console=ttyS0,115200 ignore_loglevel nokaslr nosmp nopti nospectre_v2" \
    -netdev tap,id=network0,ifname=tap1,script=no,downscript=no \
    -device e1000,netdev=network0,mac=52:54:00:00:00:02 \
    -netdev user,id=netssh0,hostfwd=tcp::10022-:22 \
    -device virtio-net-pci,netdev=netssh0 \
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
hetgpu-device=$HETGPU_DEVICE,\
hetgpu-backend=$HETGPU_BACKEND,\
id=cxl-type2-hetgpu0 \
    -device cxl-type3,bus=root_port14,persistent-memdev=cxl-mem1,lsa=cxl-lsa1,id=cxl-pmem0,sn=0x3 \
    -object memory-backend-file,id=cxl-mem1,share=on,mem-path=/tmp/cxltest.raw,size=128G \
    -object memory-backend-file,id=cxl-lsa1,share=on,mem-path=/tmp/lsa.raw,size=256M \
    -M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=4G \
    -nographic
