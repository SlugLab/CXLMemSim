#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

QEMU_BINARY=${QEMU_BINARY:-"$REPO_ROOT/lib/qemu/build/qemu-system-x86_64"}
QEMU_IMG_BINARY=${QEMU_IMG_BINARY:-"$REPO_ROOT/lib/qemu/build/qemu-img"}
SERVER_BINARY=${CXL_MEMSIM_SERVER_BINARY:-"$REPO_ROOT/build/cxlmemsim_server"}
RUN_DIR=${CXL_MEMSIM_RUN_DIR:-"$REPO_ROOT/build/qemu-vcs-dcd-gfam"}

CXL_MEMSIM_HOST=${CXL_MEMSIM_HOST:-127.0.0.1}
CXL_MEMSIM_PORT=${CXL_MEMSIM_PORT:-9999}
CXL_TRANSPORT_MODE=${CXL_TRANSPORT_MODE:-tcp}
CXL_CAPACITY_MB=${CXL_CAPACITY_MB:-10240}
CXL_DC_SIZE=${CXL_DC_SIZE:-${CXL_CAPACITY_MB}M}
CXL_FMW_SIZE=${CXL_FMW_SIZE:-10G}
CXL_DCD_GRANULARITY_MB=${CXL_DCD_GRANULARITY_MB:-1}
CXL_DCD_INITIAL_CAPACITY_MB=${CXL_DCD_INITIAL_CAPACITY_MB:-$CXL_CAPACITY_MB}
CXL_GFAM_HOSTS=${CXL_GFAM_HOSTS:-16}
CXL_GFAM_FABRIC_LATENCY=${CXL_GFAM_FABRIC_LATENCY:-80}
CXL_GFAM_BANDWIDTH=${CXL_GFAM_BANDWIDTH:-64}
CXL_DEFAULT_LATENCY=${CXL_DEFAULT_LATENCY:-400}
CXL_TOPOLOGY=${CXL_TOPOLOGY:-"$SCRIPT_DIR/topology_simple.txt"}
CXL_MEMSIM_SERVER_AUTOSTART=${CXL_MEMSIM_SERVER_AUTOSTART:-auto}
CXL_TYPE2_ENABLE=${CXL_TYPE2_ENABLE:-1}
CXL_TYPE2_MEM_SIZE=${CXL_TYPE2_MEM_SIZE:-4G}
CXL_TYPE2_CACHE_SIZE=${CXL_TYPE2_CACHE_SIZE:-128M}
CXL_TYPE2_DCD_INITIAL_SIZE=${CXL_TYPE2_DCD_INITIAL_SIZE:-$CXL_TYPE2_MEM_SIZE}
CXL_TYPE2_HOST_ID=${CXL_TYPE2_HOST_ID:-0}
CXL_TYPE2_HEAD_ID=${CXL_TYPE2_HEAD_ID:-0}

VM_MEMORY=${VM_MEMORY:-4G}
VM_MAXMEM=${VM_MAXMEM:-32G}
VM_SMP=${VM_SMP:-4}
DISK_IMAGE=${DISK_IMAGE:-./qemu.img}
DISK_FORMAT=${DISK_FORMAT:-auto}
KERNEL_IMAGE=${KERNEL_IMAGE:-./bzImage}
KERNEL_APPEND=${KERNEL_APPEND:-"root=/dev/vda rw console=ttyS0,115200 nokaslr"}
QEMU_DISK_DEVICE=${QEMU_DISK_DEVICE:-"virtio-blk-pci,drive=bootdisk,bus=pcie.0,id=bootdisk0"}
QEMU_NET_MODE=${QEMU_NET_MODE:-none}
QEMU_NETDEV=${QEMU_NETDEV:-}
QEMU_NET_DEVICE=${QEMU_NET_DEVICE:-"virtio-net-pci,netdev=net0"}

if [[ ! -x "$QEMU_BINARY" ]]; then
    echo "QEMU binary not found or not executable: $QEMU_BINARY" >&2
    exit 1
fi
if [[ ! -x "$QEMU_IMG_BINARY" ]] && command -v qemu-img >/dev/null 2>&1; then
    QEMU_IMG_BINARY=$(command -v qemu-img)
fi

if [[ -z "$DISK_IMAGE" ]]; then
    for candidate in "$REPO_ROOT/qemu.img" "$SCRIPT_DIR/qemu.img" "$HOME/qemu.img"; do
        if [[ -f "$candidate" ]]; then
            DISK_IMAGE=$candidate
            break
        fi
    done
fi

if [[ -z "$DISK_IMAGE" && -z "$KERNEL_IMAGE" ]]; then
    echo "Set DISK_IMAGE or KERNEL_IMAGE before running this script." >&2
    exit 1
fi
if [[ -n "$DISK_IMAGE" && ! -f "$DISK_IMAGE" ]]; then
    echo "DISK_IMAGE does not exist: $DISK_IMAGE" >&2
    exit 1
fi
if [[ -n "$KERNEL_IMAGE" && ! -f "$KERNEL_IMAGE" ]]; then
    echo "KERNEL_IMAGE does not exist: $KERNEL_IMAGE" >&2
    exit 1
fi

if [[ -n "$DISK_IMAGE" && "$DISK_FORMAT" == "auto" ]]; then
    if [[ -x "$QEMU_IMG_BINARY" ]]; then
        DISK_FORMAT=$("$QEMU_IMG_BINARY" info "$DISK_IMAGE" 2>/dev/null |
            sed -n 's/^file format: //p' | head -1)
    fi
    if [[ -z "$DISK_FORMAT" || "$DISK_FORMAT" == "auto" ]]; then
        if LC_ALL=C head -c 4 "$DISK_IMAGE" | grep -q '^QFI'; then
            DISK_FORMAT=qcow2
        else
            DISK_FORMAT=raw
        fi
    fi
fi

if [[ -n "$DISK_IMAGE" && -z "$KERNEL_IMAGE" && "$DISK_FORMAT" == "raw" ]]; then
    disk_type=$(file -b "$DISK_IMAGE" 2>/dev/null || true)
    if [[ "$disk_type" == *"filesystem data"* ]]; then
        echo "DISK_IMAGE is a raw filesystem, not a bootable whole-disk image: $DISK_IMAGE" >&2
        echo "Detected: $disk_type" >&2
        echo "Set KERNEL_IMAGE=/path/to/bzImage or vmlinuz and boot it as root=/dev/vda, or use a bootable disk image with a bootloader." >&2
        exit 1
    fi
fi

net_args=()
case "$QEMU_NET_MODE" in
    none|off|0)
        ;;
    passt)
        QEMU_NETDEV=${QEMU_NETDEV:-"passt,id=net0"}
        ;;
    tap)
        QEMU_NETDEV=${QEMU_NETDEV:-"tap,id=net0,ifname=${QEMU_TAP_IFNAME:-tap0},script=no,downscript=no"}
        ;;
    user)
        QEMU_NETDEV=${QEMU_NETDEV:-"user,id=net0"}
        ;;
    custom)
        if [[ -z "$QEMU_NETDEV" ]]; then
            echo "QEMU_NET_MODE=custom requires QEMU_NETDEV." >&2
            exit 1
        fi
        ;;
    *)
        echo "Unsupported QEMU_NET_MODE='$QEMU_NET_MODE'. Use none, passt, tap, user, or custom." >&2
        exit 1
        ;;
esac

if [[ -n "$QEMU_NETDEV" ]]; then
    net_backend=${QEMU_NETDEV%%,*}
    if ! "$QEMU_BINARY" -netdev help 2>/dev/null | awk '{print $1}' | grep -qx "$net_backend"; then
        echo "QEMU net backend '$net_backend' is not compiled into $QEMU_BINARY." >&2
        echo "Use QEMU_NET_MODE=none, passt, tap, or custom with a compiled backend." >&2
        exit 1
    fi
    if [[ "$net_backend" == "passt" ]] && ! command -v passt >/dev/null 2>&1; then
        echo "QEMU net backend 'passt' is compiled in, but the passt executable is not installed or not in PATH." >&2
        echo "Install passt, or run with QEMU_NET_MODE=none. For host-managed networking, use QEMU_NET_MODE=tap." >&2
        exit 1
    fi
    net_args=(-netdev "$QEMU_NETDEV" -device "$QEMU_NET_DEVICE")
fi

mkdir -p "$RUN_DIR"
truncate -s "$CXL_DC_SIZE" "$RUN_DIR/cxl-dcd0.raw"
truncate -s "$CXL_DC_SIZE" "$RUN_DIR/cxl-dcd1.raw"
truncate -s "$CXL_DC_SIZE" "$RUN_DIR/cxl-dcd2.raw"
truncate -s "$CXL_DC_SIZE" "$RUN_DIR/cxl-dcd3.raw"
truncate -s 1M "$RUN_DIR/cxl-lsa0.raw"
truncate -s 1M "$RUN_DIR/cxl-lsa1.raw"
truncate -s 1M "$RUN_DIR/cxl-lsa2.raw"
truncate -s 1M "$RUN_DIR/cxl-lsa3.raw"

server_is_up() {
    timeout 1 bash -c "</dev/tcp/$CXL_MEMSIM_HOST/$CXL_MEMSIM_PORT" \
        >/dev/null 2>&1
}

start_server() {
    if [[ ! -x "$SERVER_BINARY" ]]; then
        echo "CXLMemSim server not found or not executable: $SERVER_BINARY" >&2
        exit 1
    fi

    echo "Starting CXLMemSim server on $CXL_MEMSIM_HOST:$CXL_MEMSIM_PORT"
    "$SERVER_BINARY" \
        --comm-mode=tcp \
        --port="$CXL_MEMSIM_PORT" \
        --capacity="$CXL_CAPACITY_MB" \
        --default_latency="$CXL_DEFAULT_LATENCY" \
        --topology="$CXL_TOPOLOGY" \
        --enable-dcd \
        --dcd-granularity-mb="$CXL_DCD_GRANULARITY_MB" \
        --dcd-initial-capacity="$CXL_DCD_INITIAL_CAPACITY_MB" \
        --enable-gfam \
        --gfam-hosts="$CXL_GFAM_HOSTS" \
        --gfam-fabric-latency="$CXL_GFAM_FABRIC_LATENCY" \
        --gfam-bandwidth="$CXL_GFAM_BANDWIDTH" \
        >"$RUN_DIR/cxlmemsim-server.log" 2>&1 &
    SERVER_PID=$!

    for _ in $(seq 1 50); do
        if server_is_up; then
            echo "CXLMemSim server is ready; log: $RUN_DIR/cxlmemsim-server.log"
            return
        fi
        if ! kill -0 "$SERVER_PID" >/dev/null 2>&1; then
            echo "CXLMemSim server exited during startup." >&2
            tail -100 "$RUN_DIR/cxlmemsim-server.log" >&2 || true
            exit 1
        fi
        sleep 0.1
    done

    echo "Timed out waiting for CXLMemSim server." >&2
    tail -100 "$RUN_DIR/cxlmemsim-server.log" >&2 || true
    exit 1
}

if ! server_is_up; then
    case "$CXL_MEMSIM_SERVER_AUTOSTART" in
        1|true|yes|on|auto)
            start_server
            ;;
        *)
            echo "CXLMemSim server is not reachable and autostart is disabled." >&2
            exit 1
            ;;
    esac
fi

boot_args=()
if [[ -n "$KERNEL_IMAGE" ]]; then
    boot_args+=(-kernel "$KERNEL_IMAGE" -append "$KERNEL_APPEND")
fi
if [[ -n "$DISK_IMAGE" ]]; then
    boot_args+=(-drive "file=$DISK_IMAGE,if=none,id=bootdisk,format=$DISK_FORMAT")
    boot_args+=(-device "$QEMU_DISK_DEVICE")
fi

extra_args=()
if [[ -n "${QEMU_EXTRA_ARGS:-}" ]]; then
    extra_args=( $QEMU_EXTRA_ARGS )
fi

type2_args=()
case "$CXL_TYPE2_ENABLE" in
    1|true|yes|on)
        type2_args=(
            -device cxl-rp,port=1,bus=cxl.0,id=type2_root_port,chassis=0,slot=2
            -device "cxl-type2,bus=type2_root_port,id=cxl-type2-0,sn=200,gpu-mode=0,cache-size=$CXL_TYPE2_CACHE_SIZE,mem-size=$CXL_TYPE2_MEM_SIZE,cxlmemsim-addr=$CXL_MEMSIM_HOST,cxlmemsim-port=$CXL_MEMSIM_PORT,dcd=on,dcd-granularity=${CXL_DCD_GRANULARITY_MB}M,dcd-initial-size=$CXL_TYPE2_DCD_INITIAL_SIZE,gfam=on,gfam-hosts=$CXL_GFAM_HOSTS,gfam-host-id=$CXL_TYPE2_HOST_ID,gfam-latency-ns=$CXL_GFAM_FABRIC_LATENCY,gfam-bandwidth-mbps=$((CXL_GFAM_BANDWIDTH * 1024)),mhsld=on,mhsld-heads=$CXL_GFAM_HOSTS,mhsld-head-id=$CXL_TYPE2_HEAD_ID"
        )
        ;;
    0|false|no|off)
        ;;
    *)
        echo "Unsupported CXL_TYPE2_ENABLE='$CXL_TYPE2_ENABLE'. Use on or off." >&2
        exit 1
        ;;
esac

export CXL_TRANSPORT_MODE
export CXL_MEMSIM_HOST
export CXL_MEMSIM_PORT
export CXL_DCD_ENABLE=1
export CXL_GFAM_ENABLE=1
export CXL_MEMSIM_EARLY_INIT=1
export CXL_MEMSIM_FABRIC_REFRESH_NS=${CXL_MEMSIM_FABRIC_REFRESH_NS:-1000000000}

echo "Launching QEMU with Zettai DCD/GFAM topology"
echo "  QEMU: $QEMU_BINARY"
echo "  CXLMemSim: $CXL_MEMSIM_HOST:$CXL_MEMSIM_PORT"
echo "  DCD capacity: $CXL_CAPACITY_MB MB"
echo "  Type2 endpoint: $CXL_TYPE2_ENABLE"
echo "  Run dir: $RUN_DIR"
echo "  Network mode: $QEMU_NET_MODE"
if [[ -n "$DISK_IMAGE" ]]; then
    echo "  Disk: $DISK_IMAGE ($DISK_FORMAT)"
fi

exec "$QEMU_BINARY" \
    -enable-kvm \
    -cpu host \
    -smp "$VM_SMP" \
    -m "$VM_MEMORY",maxmem="$VM_MAXMEM",slots=8 \
    -M q35,cxl=on,cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size="$CXL_FMW_SIZE",cxl-fmw.1.targets.0=cxl.1,cxl-fmw.1.size="$CXL_FMW_SIZE" \
    "${net_args[@]}" \
    "${boot_args[@]}" \
    -object "memory-backend-file,id=cxl-mem0,share=on,mem-path=$RUN_DIR/cxl-dcd0.raw,size=$CXL_DC_SIZE" \
    -object "memory-backend-file,id=cxl-lsa0,share=on,mem-path=$RUN_DIR/cxl-lsa0.raw,size=1M" \
    -object "memory-backend-file,id=cxl-mem1,share=on,mem-path=$RUN_DIR/cxl-dcd1.raw,size=$CXL_DC_SIZE" \
    -object "memory-backend-file,id=cxl-lsa1,share=on,mem-path=$RUN_DIR/cxl-lsa1.raw,size=1M" \
    -object "memory-backend-file,id=cxl-mem2,share=on,mem-path=$RUN_DIR/cxl-dcd2.raw,size=$CXL_DC_SIZE" \
    -object "memory-backend-file,id=cxl-lsa2,share=on,mem-path=$RUN_DIR/cxl-lsa2.raw,size=1M" \
    -object "memory-backend-file,id=cxl-mem3,share=on,mem-path=$RUN_DIR/cxl-dcd3.raw,size=$CXL_DC_SIZE" \
    -object "memory-backend-file,id=cxl-lsa3,share=on,mem-path=$RUN_DIR/cxl-lsa3.raw,size=1M" \
    -object zettai,id=zettai0,usp-ppbs=2,dsp-ppbs=4,local-fm=true \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.0 \
    -device cxl-rp,port=0,bus=cxl.0,id=root_port0,chassis=0,slot=1 \
    "${type2_args[@]}" \
    -device pxb-cxl,bus_nr=22,bus=pcie.0,id=cxl.1 \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port1,chassis=1,slot=1 \
    -device cxl-upstream,port=0,sn=1234,bus=root_port0,id=us0,addr=0.0,multifunction=on,vcs=zettai0,usppb=0 \
    -device cxl-upstream,port=0,sn=5678,bus=root_port1,id=us1,addr=0.0,multifunction=on,vcs=zettai0,usppb=1 \
    -device cxl-switch-mailbox-cci,bus=root_port0,addr=0.3,target=zettai0 \
    -device cxl-downstream,port=0,bus=us0,id=swport0,slot=3 \
    -device cxl-downstream,port=1,bus=us0,id=swport1,slot=4 \
    -device cxl-downstream,port=0,bus=us1,id=swport2,slot=7 \
    -device cxl-downstream,port=1,bus=us1,id=swport3,slot=8 \
    -device cxl-type3,volatile-dc-memdev=cxl-mem0,lsa=cxl-lsa0,id=cxl-dcd0,sn=101,num-dc-regions=8,vcs=zettai0,dsppb=0,memsim-dcd=on,memsim-gfam=on,memsim-gfam-host-id=0 \
    -device cxl-type3,volatile-dc-memdev=cxl-mem1,lsa=cxl-lsa1,id=cxl-dcd1,sn=102,num-dc-regions=8,vcs=zettai0,dsppb=1,memsim-dcd=on,memsim-gfam=on,memsim-gfam-host-id=1 \
    -device cxl-type3,volatile-dc-memdev=cxl-mem2,lsa=cxl-lsa2,id=cxl-dcd2,sn=103,num-dc-regions=8,vcs=zettai0,dsppb=2,memsim-dcd=on,memsim-gfam=on,memsim-gfam-host-id=2 \
    -device cxl-type3,volatile-dc-memdev=cxl-mem3,lsa=cxl-lsa3,id=cxl-dcd3,sn=104,num-dc-regions=8,vcs=zettai0,dsppb=3,memsim-dcd=on,memsim-gfam=on,memsim-gfam-host-id=3 \
    -nographic \
    "${extra_args[@]}"
