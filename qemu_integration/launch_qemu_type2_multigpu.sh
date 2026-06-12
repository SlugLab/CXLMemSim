#!/bin/bash
# Launch QEMU with N CXL Type-2 accelerator contexts attached to one host VM.
# Used by the Splash multi-GPU scaling sweep.
#
# Environment:
#   NUM_TYPE2        - Number of Type-2 accelerators to attach (1, 2, 4, 6, 8).
#                      Default: 1.
#   QEMU_BINARY      - Path to qemu-system-x86_64.
#   DISK_IMAGE       - Path to the guest rootfs image (relative CWD).
#   KERNEL_IMAGE     - Path to bzImage.
#   SSH_PORT         - hostfwd guest:22 -> host:${SSH_PORT}. Default 10022.
#   HETGPU_BACKEND   - 0 auto / 3 nvidia / 5 simulation. Default 5.
#   HETGPU_LIB       - Path to libnvcuda.so.
#   CXL_TYPE2_CACHE_SIZE / CXL_TYPE2_MEM_SIZE - per-device sizes.
#   DIRECTORY_ENTRIES - snoop filter capacity (0 uses device default).
#   MONITOR_SOCK     - QEMU monitor unix socket path.
#   VM_LOG           - File to redirect serial console + stderr.

set -e

NUM_TYPE2=${NUM_TYPE2:-1}
QEMU_BINARY=${QEMU_BINARY:-/home/victoryang00/CXLMemSim/lib/qemu/build/qemu-system-x86_64}
DISK_IMAGE=${DISK_IMAGE:-qemu1.img}
DISK_IMAGE_PATH=${DISK_IMAGE_PATH:-}
# If no absolute path provided, default to <repo>/build/<DISK_IMAGE> which is
# the canonical location, independent of the invoker's working directory.
if [ -z "${DISK_IMAGE_PATH}" ]; then
    DISK_IMAGE_PATH="/home/victoryang00/CXLMemSim/build/${DISK_IMAGE}"
fi
KERNEL_IMAGE=${KERNEL_IMAGE:-/home/victoryang00/cxl/arch/x86/boot/bzImage}
SSH_PORT=${SSH_PORT:-10022}
HETGPU_BACKEND=${HETGPU_BACKEND:-5}
HETGPU_LIB=${HETGPU_LIB:-/home/victoryang00/hetGPU/target/debug/libnvcuda.so}
CXL_TYPE2_CACHE_SIZE=${CXL_TYPE2_CACHE_SIZE:-128M}
CXL_TYPE2_MEM_SIZE=${CXL_TYPE2_MEM_SIZE:-4G}
DIRECTORY_ENTRIES=${DIRECTORY_ENTRIES:-0}
MONITOR_SOCK=${MONITOR_SOCK:-/tmp/qemu-mon.sock}
VM_LOG=${VM_LOG:-/home/victoryang00/CXLMemSim/artifact/splash_sweep/vm1.log}
CXL_MEMSIM_HOST=${CXL_MEMSIM_HOST:-127.0.0.1}
CXL_MEMSIM_PORT=${CXL_MEMSIM_PORT:-9999}

if ! [[ "$NUM_TYPE2" =~ ^[0-9]+$ ]] || [ "$NUM_TYPE2" -lt 1 ] || [ "$NUM_TYPE2" -gt 8 ]; then
    echo "NUM_TYPE2 must be an integer between 1 and 8" >&2
    exit 1
fi

RP_OPTS=()
T2_OPTS=()
for i in $(seq 0 $((NUM_TYPE2-1))); do
    PORT=$((13 + i))
    RP_ID="root_port${PORT}"
    T2_ID="cxl-type2-hetgpu${i}"
    SN=$(printf "0x%x" $((0x10 + i)))
    RP_OPTS+=( -device "cxl-rp,port=${i},bus=cxl.1,id=${RP_ID},chassis=0,slot=${i}" )
    T2_OPTS+=( -device "cxl-type2,bus=${RP_ID},cache-size=${CXL_TYPE2_CACHE_SIZE},mem-size=${CXL_TYPE2_MEM_SIZE},sn=${SN},cxlmemsim-addr=${CXL_MEMSIM_HOST},cxlmemsim-port=${CXL_MEMSIM_PORT},coherency-enabled=true,gpu-mode=2,hetgpu-lib=${HETGPU_LIB},hetgpu-device=0,hetgpu-backend=${HETGPU_BACKEND},directory-entries=${DIRECTORY_ENTRIES},id=${T2_ID}" )
done

exec "${QEMU_BINARY}" \
    --enable-kvm -cpu qemu64,+xsave,+rdtscp,+avx,+avx2,+sse4.1,+sse4.2,+avx512f,+avx512dq,+avx512ifma,+avx512cd,+avx512bw,+avx512vl,+avx512vbmi,+clflushopt \
    -kernel "${KERNEL_IMAGE}" \
    -append "root=/dev/vda rw console=ttyS0,115200 nokaslr systemd.mask=cxl-numa-setup.service" \
    -netdev "user,id=net0,hostfwd=tcp::${SSH_PORT}-:22" \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:00:00:02 \
    -device virtio-rng-pci \
    -drive file="${DISK_IMAGE_PATH:-./${DISK_IMAGE}}",if=none,id=disk0,format=raw \
    -device virtio-blk-pci,drive=disk0,bus=pcie.0 \
    -M q35,cxl=on -m 16G,maxmem=32G,slots=8 -smp 4 \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
    "${RP_OPTS[@]}" \
    "${T2_OPTS[@]}" \
    -monitor "unix:${MONITOR_SOCK},server,nowait" \
    -D /dev/null \
    -nographic > "${VM_LOG}" 2>&1
