#!/bin/bash

# Launch QEMU with CXL Type 2 Device for GPU Passthrough
# This script demonstrates using the CXL Type 2 device which provides:
# - Cache coherency (Type 1 feature)
# - Device-attached memory (Type 3 feature)
# - GPU passthrough with CPU-GPU coherency

QEMU_BINARY=/usr/local/bin/qemu-system-x86_64
CXL_MEMSIM_HOST=${CXL_MEMSIM_HOST:-127.0.0.1}
CXL_MEMSIM_PORT=${CXL_MEMSIM_PORT:-9999}
VM_MEMORY=${VM_MEMORY:-2G}
CXL_MEMORY=${CXL_MEMORY:-4G}
DISK_IMAGE=${DISK_IMAGE:-qemu1.img}

# Type 2 device configuration
CXL_TYPE2_CACHE_SIZE=${CXL_TYPE2_CACHE_SIZE:-128M}
CXL_TYPE2_MEM_SIZE=${CXL_TYPE2_MEM_SIZE:-4G}

# GPU passthrough configuration (optional)
# Set this to your GPU's PCI address for actual passthrough
# Example: GPU_DEVICE="0000:01:00.0"
GPU_DEVICE=${GPU_DEVICE:-""}

# Communication mode: tcp or shm (shared memory)
export CXL_TRANSPORT_MODE=${CXL_TRANSPORT_MODE:-shm}
export CXL_MEMSIM_HOST=$CXL_MEMSIM_HOST
export CXL_MEMSIM_PORT=$CXL_MEMSIM_PORT

echo "==================================================================="
echo "CXL Type 2 Device (GPU Passthrough Forwarder) Configuration"
echo "==================================================================="
echo "CXLMemSim Server: $CXL_MEMSIM_HOST:$CXL_MEMSIM_PORT"
echo "Transport Mode: $CXL_TRANSPORT_MODE"
echo "Type 2 Cache Size: $CXL_TYPE2_CACHE_SIZE"
echo "Type 2 Device Memory: $CXL_TYPE2_MEM_SIZE"
echo "GPU Device: ${GPU_DEVICE:-'Not configured (simulation mode)'}"
echo "==================================================================="

# Build QEMU command
QEMU_CMD="$QEMU_BINARY \
    --enable-kvm \
    -cpu qemu64,+xsave,+rdtscp,+avx,+avx2,+sse4.1,+sse4.2,+avx512f,+avx512dq,+avx512ifma,+avx512cd,+avx512bw,+avx512vl,+avx512vbmi,+clflushopt \
    -kernel ./bzImage \
    -append \"root=/dev/sda rw console=ttyS0,115200 ignore_loglevel nokaslr nosmp nopti nospectre_v2 mem=2G\" \
    -netdev tap,id=network0,ifname=tap1,script=no,downscript=no \
    -device e1000,netdev=network0,mac=52:54:00:00:00:02 \
    -netdev user,id=netssh0,hostfwd=tcp::10022-:22 \
    -device virtio-net-pci,netdev=netssh0 \
    -device virtio-rng-pci \
    -drive file=./$DISK_IMAGE,index=0,media=disk,format=raw \
    -M q35,cxl=on \
    -m 16G,maxmem=32G,slots=8 \
    -smp 4 \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=0 \
    -device cxl-rp,port=1,bus=cxl.1,id=root_port14,chassis=0,slot=1"

# Add CXL Type 2 device with coherent memory and GPU passthrough support
QEMU_CMD="$QEMU_CMD \
    -device cxl-type2,bus=root_port13,\
cache-size=$CXL_TYPE2_CACHE_SIZE,\
mem-size=$CXL_TYPE2_MEM_SIZE,\
sn=0x2,\
cxlmemsim-addr=$CXL_MEMSIM_HOST,\
cxlmemsim-port=$CXL_MEMSIM_PORT,\
coherency-enabled=true,\
id=cxl-type2-gpu0"

# Add GPU device parameter if specified
if [ -n "$GPU_DEVICE" ]; then
    QEMU_CMD="$QEMU_CMD,gpu-device=$GPU_DEVICE"
    echo "GPU passthrough enabled for: $GPU_DEVICE"
fi

# Add traditional Type 3 memory device for comparison
QEMU_CMD="$QEMU_CMD \
    -device cxl-type3,bus=root_port14,persistent-memdev=cxl-mem1,lsa=cxl-lsa1,id=cxl-pmem0,sn=0x3 \
    -object memory-backend-file,id=cxl-mem1,share=on,mem-path=/tmp/cxltest.raw,size=128G \
    -object memory-backend-file,id=cxl-lsa1,share=on,mem-path=/tmp/lsa.raw,size=256M \
    -M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=4G \
    -nographic"

echo ""
echo "Starting QEMU with CXL Type 2 device..."
echo ""

# Execute QEMU
exec $QEMU_CMD
