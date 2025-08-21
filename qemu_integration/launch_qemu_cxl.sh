#!/bin/bash

QEMU_BINARY=$PWD/../lib/qemu/build/qemu-system-x86_64
CXL_MEMSIM_HOST=${CXL_MEMSIM_HOST:-127.0.0.1}
CXL_MEMSIM_PORT=${CXL_MEMSIM_PORT:-9999}
VM_MEMORY=${VM_MEMORY:-2G}
CXL_MEMORY=${CXL_MEMORY:-4G}
DISK_IMAGE=${DISK_IMAGE:-plucky-server-cloudimg-amd64.img}

export LD_PRELOAD=/usr/local/lib/libCXLMemSim.so
export CXL_MEMSIM_HOST=$CXL_MEMSIM_HOST
export CXL_MEMSIM_PORT=$CXL_MEMSIM_PORT

exec $QEMU_BINARY \
    --enable-kvm --cpu host  \
    -kernel /root/tdx-linux/arch/x86/boot/bzImage \
    -append "root=/dev/sda rw console=ttyS0,115200 ignore_loglevel nokaslr" \
    -netdev user,id=network0,hostfwd=tcp::2024-:22 \
    -device e1000,netdev=network0 \
    -drive file=/home/victoryang00/CXLMemSim/build/qemu.img,index=0,media=disk,format=raw \
   -M q35,cxl=on -m 4G,maxmem=8G,slots=8 -smp 4 \
    -object memory-backend-file,id=cxl-mem1,share=on,mem-path=/tmp/cxltest.raw,size=256M \
    -object memory-backend-file,id=cxl-lsa1,share=on,mem-path=/tmp/lsa.raw,size=256M \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=2 \
    -device cxl-type3,bus=root_port13,persistent-memdev=cxl-mem1,lsa=cxl-lsa1,id=cxl-pmem0,sn=0x1 \
    -M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=4G \
    -nographic
