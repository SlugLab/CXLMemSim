#!/bin/bash

QEMU_BINARY=/usr/local/bin/qemu-system-x86_64
CXL_MEMSIM_HOST=${CXL_MEMSIM_HOST:-127.0.0.1}
CXL_MEMSIM_PORT=${CXL_MEMSIM_PORT:-9999}
VM_MEMORY=${VM_MEMORY:-2G}
CXL_MEMORY=${CXL_MEMORY:-4G}
DISK_IMAGE=${DISK_IMAGE:-plucky-server-cloudimg-amd64.img}

# Enable SHM mode with lock-free coherency
export CXL_TRANSPORT_MODE=shm
export CXL_HOST_ID=0
exec $QEMU_BINARY \
    --enable-kvm -cpu host  \
    -m 160G,maxmem=200G,slots=8 \
    -smp 4 \
    -M q35,cxl=on \
    -kernel /home/victoryang00/open-gpu-kernel-modules/bzImage \
    -append "root=/dev/sda rw console=ttyS0,115200 nokaslr" \
    -drive file=./qemu.img,index=0,media=disk,format=raw \
    -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:00:00:01 \
    -fsdev local,security_model=none,id=fsdev0,path=/dev/shm \
    -device virtio-9p-pci,id=fs0,fsdev=fsdev0,mount_tag=hostshm,bus=pcie.0 \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=0 \
    -device cxl-rp,port=1,bus=cxl.1,id=root_port14,chassis=0,slot=1 \
    -device cxl-type3,bus=root_port13,persistent-memdev=cxl-mem1,lsa=cxl-lsa1,id=cxl-pmem0,sn=0x1 \
    -device cxl-type1,bus=root_port14,size=1G,cache-size=64M \
    -device virtio-cxl-accel-pci,bus=pcie.0 \
    -object memory-backend-file,id=cxl-mem1,share=on,mem-path=/dev/shm/cxlmemsim_shared,size=1G \
    -object memory-backend-file,id=cxl-lsa1,share=on,mem-path=/dev/shm/lsa0.raw,size=1G \
    -M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=4G \
    -nographic -device vfio-pci,host=0000:b0:00.0,bus=pcie.0,id=gpu0,x-pci-vendor-id=0x10de