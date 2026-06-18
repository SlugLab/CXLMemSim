#!/bin/bash
set -euo pipefail

QEMU_BINARY=${QEMU_BINARY:-/usr/local/bin/qemu-system-x86_64}
ROOT=${ROOT:-/home/victoryang00/CXLMemSim}
KERNEL=${KERNEL:-$ROOT/build/bzImage}
DISK_IMAGE=${DISK_IMAGE:-$ROOT/build/qemu.img}
DISK_FORMAT=${DISK_FORMAT:-raw}
VM_MEMORY=${VM_MEMORY:-8G}
VM_MAX_MEMORY=${VM_MAX_MEMORY:-16G}
SMP=${SMP:-4}
SSH_PORT=${SSH_PORT:-10022}
NET_MAC=${NET_MAC:-52:54:00:00:10:22}
CXL_BACKING=${CXL_BACKING:-/dev/shm/cxlmemsim_shared}
CXL_LSA=${CXL_LSA:-/dev/shm/lsa0.raw}
CXL_BACKING_SIZE=${CXL_BACKING_SIZE:-1G}
CXL_LSA_SIZE=${CXL_LSA_SIZE:-256M}
CXL_FMW_SIZE=${CXL_FMW_SIZE:-4G}

truncate -s "$CXL_BACKING_SIZE" "$CXL_BACKING"
truncate -s "$CXL_LSA_SIZE" "$CXL_LSA"

export CXL_TRANSPORT_MODE=${CXL_TRANSPORT_MODE:-shm}
export CXL_PGAS_SHM=${CXL_PGAS_SHM:-/cxlmemsim_pgas}
export CXL_HOST_ID=${CXL_HOST_ID:-0}
export CXL_MEMSIM_HOST=${CXL_MEMSIM_HOST:-127.0.0.1}
export CXL_MEMSIM_PORT=${CXL_MEMSIM_PORT:-9999}

exec "$QEMU_BINARY" \
    --enable-kvm \
    -cpu qemu64,+xsave,+rdtscp,+avx,+avx2,+sse4.1,+sse4.2,+clflushopt \
    -m "$VM_MEMORY",maxmem="$VM_MAX_MEMORY",slots=8 \
    -smp "$SMP" \
    -M q35,cxl=on \
    -kernel "$KERNEL" \
    -append "root=/dev/sda rw console=ttyS0,115200 nokaslr" \
    -drive file="$DISK_IMAGE",index=0,media=disk,format="$DISK_FORMAT" \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:"$SSH_PORT"-:22 \
    -device virtio-net-pci,netdev=net0,mac="$NET_MAC" \
    -fsdev local,security_model=none,id=fsdev0,path="$ROOT" \
    -device virtio-9p-pci,id=fs0,fsdev=fsdev0,mount_tag=hostrepo,bus=pcie.0 \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=0 \
    -device cxl-type3,bus=root_port13,persistent-memdev=cxl-mem1,lsa=cxl-lsa1,id=cxl-pmem0,sn=0x1 \
    -object memory-backend-file,id=cxl-mem1,share=on,mem-path="$CXL_BACKING",size="$CXL_BACKING_SIZE" \
    -object memory-backend-file,id=cxl-lsa1,share=on,mem-path="$CXL_LSA",size="$CXL_LSA_SIZE" \
    -M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size="$CXL_FMW_SIZE" \
    -nographic
