#!/bin/bash

QEMU_BINARY=/usr/local/bin/qemu-system-x86_64
CXL_MEMSIM_HOST=${CXL_MEMSIM_HOST:-127.0.0.1}
CXL_MEMSIM_PORT=${CXL_MEMSIM_PORT:-9999}
VM_MEMORY=${VM_MEMORY:-2G}
CXL_MEMORY=${CXL_MEMORY:-4G}
DISK_IMAGE=${DISK_IMAGE:-plucky-server-cloudimg-amd64.img}

# Enable RDMA mode
export CXL_TRANSPORT_MODE=shm
# Also set TCP fallback
export CXL_MEMSIM_HOST=127.0.0.1
export CXL_MEMSIM_PORT=9999
exec $QEMU_BINARY \
    --enable-kvm -cpu qemu64,+xsave,+rdtscp,+avx,+avx2,+sse4.1,+sse4.2,+avx512f,+avx512dq,+avx512ifma,+avx512cd,+avx512bw,+avx512vl,+avx512vbmi,+clflushopt  \
    -m 16G,maxmem=32G,slots=8 \
    -smp 4 \
    -M q35,cxl=on \
    -kernel ./bzImage \
    -append "root=/dev/sda rw console=ttyS0,115200 nokaslr" \
    -drive file=./qemu2.img,index=0,media=disk,format=raw \
    -netdev tap,id=net0,ifname=tap2,script=no,downscript=no \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:00:00:03 \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=0 \
    -device cxl-rp,port=1,bus=cxl.1,id=root_port14,chassis=0,slot=1 \
    -device cxl-type3,bus=root_port13,persistent-memdev=cxl-mem1,lsa=cxl-lsa1,id=cxl-pmem0,sn=0x1 \
    -device cxl-type1,bus=root_port14,size=256M,cache-size=64M \
    -device virtio-cxl-accel-pci,bus=pcie.0 \
    -object memory-backend-file,id=cxl-mem1,share=on,mem-path=/tmp/cxltest.raw,size=256M \
    -object memory-backend-file,id=cxl-lsa1,share=on,mem-path=/tmp/lsa.raw,size=256M \
    -M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=4G \
    -nographic