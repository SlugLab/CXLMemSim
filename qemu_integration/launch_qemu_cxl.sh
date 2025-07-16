#!/bin/bash

QEMU_BINARY=$PWD/../lib/qemu/build/qemu-system-x86_64
CXL_MEMSIM_HOST=${CXL_MEMSIM_HOST:-127.0.0.1}
CXL_MEMSIM_PORT=${CXL_MEMSIM_PORT:-9999}
VM_MEMORY=${VM_MEMORY:-2G}
CXL_MEMORY=${CXL_MEMORY:-4G}
DISK_IMAGE=${DISK_IMAGE:-disk.qcow2}

export LD_PRELOAD=/usr/local/lib/libCXLMemSim.so
export CXL_MEMSIM_HOST=$CXL_MEMSIM_HOST
export CXL_MEMSIM_PORT=$CXL_MEMSIM_PORT

QEMU_CMD="$QEMU_BINARY \
    -M q35,cxl=on \
    -m $VM_MEMORY \
    -smp 4 \
    -enable-kvm \
    -drive file=$DISK_IMAGE,if=virtio,format=qcow2 \
    -object memory-backend-file,id=cxl-mem1,size=$CXL_MEMORY,mem-path=/dev/shm/cxl-mem,share=on \
    -device pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52 \
    -device cxl-rp,id=rp0,bus=cxl.0,chassis=0,port=0,slot=0 \
    -device cxl-type3,bus=rp0,volatile-memdev=cxl-mem1,id=cxl-vmem0 \
    -M cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=8G \
    -nographic"

echo "Starting QEMU with CXL memory connected to $CXL_MEMSIM_HOST:$CXL_MEMSIM_PORT"
echo "Command: $QEMU_CMD"
echo ""

exec $QEMU_CMD