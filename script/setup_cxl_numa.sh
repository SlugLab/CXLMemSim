#!/usr/bin/env bash

# Guest-side CXL setup helper.
#
# Default mode is suitable for volatile CXL Dynamic Capacity Device (DCD)
# memory: create a CXL RAM region and then create a device-dax instance from
# that region. Legacy pmem namespace creation is still available through
# CXL_CREATE_NDCTL_NAMESPACE=1.

set -euo pipefail

LOG_FILE=${LOG_FILE:-/var/log/cxl_numa_setup.log}
REGION_SIZE=${REGION_SIZE:-${CXL_REGION_SIZE:-256M}}
INTERLEAVE_GRANULARITY=${INTERLEAVE_GRANULARITY:-1024}
CXL_REGION_TYPE=${CXL_REGION_TYPE:-ram}
CXL_MEMDEV=${CXL_MEMDEV:-${ZETTAI_MEMDEV:-}}
CXL_DECODER=${CXL_DECODER:-${ZETTAI_DECODER:-}}
CXL_CREATE_DAX=${CXL_CREATE_DAX:-${ZETTAI_CREATE_DAX:-1}}
CXL_DAX_MODE=${CXL_DAX_MODE:-devdax}
CXL_TOUCH_DAX=${CXL_TOUCH_DAX:-${ZETTAI_TOUCH_DAX:-0}}
CXL_CREATE_NDCTL_NAMESPACE=${CXL_CREATE_NDCTL_NAMESPACE:-0}
CXL_CONFIGURE_NET=${CXL_CONFIGURE_NET:-0}
CXL_HOST_ID=${CXL_HOST_ID:-${ZETTAI_HOST_ID:-0}}
CXL_NET_IFACE=${CXL_NET_IFACE:-auto}
CXL_NET_BASE=${CXL_NET_BASE:-192.168.100}
CXL_NET_PREFIX=${CXL_NET_PREFIX:-24}
CXL_NET_HOST_OFFSET=${CXL_NET_HOST_OFFSET:-10}
if [[ -z "${CXL_NET_ADDR:-}" ]]; then
    if [[ "$CXL_HOST_ID" =~ ^[0-9]+$ && "$CXL_NET_HOST_OFFSET" =~ ^[0-9]+$ ]]; then
        CXL_NET_ADDR="${CXL_NET_BASE}.$((CXL_NET_HOST_OFFSET + CXL_HOST_ID))/${CXL_NET_PREFIX}"
    else
        CXL_NET_ADDR="${CXL_NET_BASE}.${CXL_NET_HOST_OFFSET}/${CXL_NET_PREFIX}"
    fi
fi
CXL_NET_GW=${CXL_NET_GW:-192.168.100.1}
CXL_NET_WAIT_RETRIES=${CXL_NET_WAIT_RETRIES:-10}
CXL_NET_WAIT_DELAY=${CXL_NET_WAIT_DELAY:-0.2}
MAX_RETRIES=${MAX_RETRIES:-20}
RETRY_DELAY=${RETRY_DELAY:-2}

usage() {
    cat <<'EOF'
Usage: setup_cxl_numa.sh

Run inside the Linux guest after CXL devices are visible. For a Zettai VCS DCD
topology, bind a hidden endpoint and add dynamic capacity from the host first.

Environment:
  REGION_SIZE / CXL_REGION_SIZE  Region size, default: 256M
  CXL_REGION_TYPE                CXL region type, default: ram
  INTERLEAVE_GRANULARITY         cxl create-region granularity, default: 1024
  CXL_MEMDEV / ZETTAI_MEMDEV     Override memdev, e.g. mem0
  CXL_DECODER / ZETTAI_DECODER   Override root decoder, e.g. decoder0.0
  CXL_CREATE_DAX                 Create/use a dax device, default: 1
  CXL_DAX_MODE                   devdax, system-ram, or none; default: devdax
  CXL_TOUCH_DAX                  mmap and touch /dev/daxX.Y, default: 0
  CXL_CREATE_NDCTL_NAMESPACE     Legacy ndctl namespace path, default: 0
  CXL_CONFIGURE_NET              Configure static guest network, default: 0
  CXL_HOST_ID                    Node id used for default IP, default: 0
  CXL_NET_IFACE                  Interface name or auto, default: auto
  CXL_NET_ADDR                   Static address, default: 192.168.100.(10+CXL_HOST_ID)/24
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE" >&2
}

need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        log "ERROR: Missing required command: $1"
        exit 1
    fi
}

json_query() {
    if command -v jq >/dev/null 2>&1; then
        jq -r "$1" 2>/dev/null || true
    else
        cat >/dev/null
    fi
}

load_modules() {
    modprobe cxl_core 2>/dev/null || true
    modprobe cxl_pci 2>/dev/null || true
    modprobe cxl_acpi 2>/dev/null || true
    modprobe cxl_port 2>/dev/null || true
    modprobe cxl_mem 2>/dev/null || true
    modprobe dax 2>/dev/null || true
    modprobe device_dax 2>/dev/null || true
    modprobe kmem 2>/dev/null || true
}

detect_memdev() {
    if [[ -n "$CXL_MEMDEV" ]]; then
        echo "$CXL_MEMDEV"
        return 0
    fi

    local memdev=""
    memdev=$(cxl list -M 2>/dev/null |
        json_query '.. | objects | select(has("memdev")) | .memdev' |
        head -1)
    if [[ -z "$memdev" ]]; then
        memdev=$(cxl list -M 2>/dev/null |
            sed -n 's/.*"memdev"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' |
            head -1)
    fi
    echo "$memdev"
}

wait_for_cxl_device() {
    local retries=0

    while [[ $retries -lt $MAX_RETRIES ]]; do
        CXL_MEMDEV=$(detect_memdev)
        if [[ -n "$CXL_MEMDEV" ]]; then
            log "CXL memdev detected: $CXL_MEMDEV"
            return 0
        fi
        log "Waiting for CXL memdev... (attempt $((retries + 1))/$MAX_RETRIES)"
        sleep "$RETRY_DELAY"
        retries=$((retries + 1))
    done

    cat >&2 <<'EOF'
No CXL memdev is visible in the guest.

For a Zettai VCS DCD topology, this usually means the endpoint is still hidden
behind the switch. From the host, bind a vPPB and add dynamic capacity first:

  python3 qemu_integration/zettai_host_dcd_gfam_test.py --bind --add --query

Then rerun setup_cxl_numa.sh in the guest.
EOF
    log "ERROR: CXL memdev not found after $MAX_RETRIES attempts"
    return 1
}

detect_decoder() {
    if [[ -n "$CXL_DECODER" ]]; then
        echo "$CXL_DECODER"
        return 0
    fi

    local decoder=""
    decoder=$(cxl list -B -D 2>/dev/null |
        json_query '.. | objects | select(has("decoder")) |
                    select(.decoder | test("^decoder0\\.")) |
                    select((.volatile_capable == true) or (.pmem_capable == true)) |
                    select((.max_available_extent // 0) > 0) |
                    .decoder' |
        head -1)
    if [[ -z "$decoder" ]]; then
        decoder=$(cxl list -B -D 2>/dev/null |
            sed -n 's/.*"decoder"[[:space:]]*:[[:space:]]*"\(decoder0\.[^"]*\)".*/\1/p' |
            head -1)
    fi
    echo "$decoder"
}

detect_region() {
    local region=""

    region=$(cxl list -R 2>/dev/null |
        json_query '.. | objects | select(has("region")) | .region' |
        head -1)
    if [[ -z "$region" ]]; then
        region=$(cxl list -R 2>/dev/null |
            sed -n 's/.*"region"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' |
            head -1)
    fi
    echo "$region"
}

detect_daxdev() {
    local region=$1
    local daxdev=""

    daxdev=$(daxctl list -r "$region" 2>/dev/null |
        json_query '.. | objects | select(has("chardev")) | .chardev' |
        head -1)
    if [[ -z "$daxdev" ]]; then
        daxdev=$(daxctl list -r "$region" 2>/dev/null |
            sed -n 's/.*"chardev"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' |
            head -1)
    fi
    echo "$daxdev"
}

setup_cxl_region() {
    local region=""

    region=$(detect_region)
    if [[ -n "$region" ]]; then
        log "Using existing CXL region: $region"
        echo "$region"
        return 0
    fi

    CXL_DECODER=$(detect_decoder)
    if [[ -z "$CXL_DECODER" ]]; then
        log "ERROR: Could not find usable root decoder; set CXL_DECODER=decoderX.Y"
        return 1
    fi

    log "Creating CXL $CXL_REGION_TYPE region: memdev=$CXL_MEMDEV decoder=$CXL_DECODER size=$REGION_SIZE"
    cxl create-region -m -t "$CXL_REGION_TYPE" -d "$CXL_DECODER" \
        -w 1 -g "$INTERLEAVE_GRANULARITY" -s "$REGION_SIZE" "$CXL_MEMDEV" \
        2>&1 | tee -a "$LOG_FILE" >&2
    udevadm settle 2>/dev/null || true

    region=$(detect_region)
    if [[ -z "$region" ]]; then
        log "ERROR: Region creation did not produce a CXL region"
        return 1
    fi

    log "Created CXL region: $region"
    echo "$region"
}

setup_ndctl_namespace() {
    local region=$1

    if [[ "$CXL_CREATE_NDCTL_NAMESPACE" != 1 ]]; then
        return 0
    fi
    if ! command -v ndctl >/dev/null 2>&1; then
        log "ndctl is not installed; skipping legacy namespace creation"
        return 0
    fi
    if ndctl list -N 2>/dev/null | grep -q '"dev":"namespace'; then
        log "NVDIMM namespace already exists"
        return 0
    fi

    log "Creating legacy ndctl dax namespace on $region"
    ndctl create-namespace -m dax -r "$region" 2>&1 | tee -a "$LOG_FILE" || true
}

setup_dax_device() {
    local region=$1
    local daxdev=""

    if [[ "$CXL_CREATE_DAX" != 1 || "$CXL_DAX_MODE" == "none" ]]; then
        return 0
    fi
    if ! command -v daxctl >/dev/null 2>&1; then
        log "daxctl is not installed; skipping DAX device setup"
        return 0
    fi

    daxdev=$(detect_daxdev "$region")
    if [[ -z "$daxdev" ]]; then
        log "Creating device-dax instance for $region"
        daxctl create-device -r "$region" 2>&1 | tee -a "$LOG_FILE" || true
        udevadm settle 2>/dev/null || true
        daxdev=$(detect_daxdev "$region")
    fi

    if [[ -z "$daxdev" ]]; then
        log "WARNING: No DAX chardev found for $region"
        return 0
    fi

    log "Using DAX device: /dev/$daxdev"

    if [[ "$CXL_DAX_MODE" == "system-ram" ]]; then
        log "Reconfiguring $daxdev as system-ram"
        daxctl reconfigure-device --mode=system-ram "$daxdev" \
            2>&1 | tee -a "$LOG_FILE" || true
        return 0
    fi

    if [[ "$CXL_TOUCH_DAX" == 1 ]]; then
        touch_dax "/dev/$daxdev"
    fi
}

touch_dax() {
    local path=$1

    log "Touching $path to generate CXL.mem/GFAM traffic"
    python3 - "$path" <<'PY' 2>&1 | tee -a "$LOG_FILE"
import mmap
import os
import sys

path = sys.argv[1]
size = os.path.getsize(path)
if size <= 0:
    size = 256 * 1024 * 1024

fd = os.open(path, os.O_RDWR | getattr(os, "O_SYNC", 0))
try:
    mm = mmap.mmap(fd, size, mmap.MAP_SHARED,
                   mmap.PROT_READ | mmap.PROT_WRITE)
    checksum = 0
    for off in range(0, size, 4096):
        mm[off:off + 1] = b"Z"
    for off in range(0, size, 4096):
        checksum += mm[off]
    mm.close()
    print(f"touched {size} bytes via {path}; checksum={checksum}")
finally:
    os.close(fd)
PY
}

configure_network() {
    if [[ "$CXL_CONFIGURE_NET" != 1 ]]; then
        return 0
    fi

    if ! command -v ip >/dev/null 2>&1; then
        log "WARNING: ip command is not installed; skipping network setup"
        return 0
    fi

    local iface=""
    if ! iface=$(wait_for_net_iface); then
        log "WARNING: no network interface available for CXL_NET_IFACE=$CXL_NET_IFACE; skipping network setup"
        return 0
    fi

    log "Configuring guest network: iface=$iface addr=$CXL_NET_ADDR gw=$CXL_NET_GW"
    if ! ip link set "$iface" up; then
        log "WARNING: failed to bring $iface up; skipping network setup"
        return 0
    fi

    ip addr add "$CXL_NET_ADDR" dev "$iface" 2>/dev/null || true
    if [[ -n "$CXL_NET_GW" ]]; then
        ip route add default via "$CXL_NET_GW" 2>/dev/null || true
    fi
}

detect_net_iface() {
    if [[ "$CXL_NET_IFACE" != "auto" ]]; then
        if ip link show dev "$CXL_NET_IFACE" >/dev/null 2>&1; then
            echo "$CXL_NET_IFACE"
            return 0
        fi
        return 1
    fi

    local iface=""
    iface=$(ip -o link show up 2>/dev/null |
        awk -F': ' '$2 != "lo" { sub(/@.*/, "", $2); print $2; exit }')
    if [[ -z "$iface" ]]; then
        iface=$(ip -o link show 2>/dev/null |
            awk -F': ' '$2 != "lo" { sub(/@.*/, "", $2); print $2; exit }')
    fi

    if [[ -n "$iface" ]]; then
        echo "$iface"
        return 0
    fi
    return 1
}

wait_for_net_iface() {
    local retries=0
    local iface=""

    while [[ $retries -lt $CXL_NET_WAIT_RETRIES ]]; do
        if iface=$(detect_net_iface); then
            echo "$iface"
            return 0
        fi
        log "Waiting for guest network interface... (attempt $((retries + 1))/$CXL_NET_WAIT_RETRIES)"
        sleep "$CXL_NET_WAIT_DELAY"
        retries=$((retries + 1))
    done

    return 1
}

main() {
    local region=""

    need cxl
    log "Starting guest CXL setup"
    log "CXL topology before setup:"
    cxl list -B -D -P -M -u 2>&1 | tee -a "$LOG_FILE" || true

    load_modules
    wait_for_cxl_device
    region=$(setup_cxl_region)
    setup_ndctl_namespace "$region"
    setup_dax_device "$region"
    configure_network

    log "Final CXL configuration:"
    cxl list 2>&1 | tee -a "$LOG_FILE" || true
    if command -v daxctl >/dev/null 2>&1; then
        log "Final DAX configuration:"
        daxctl list 2>&1 | tee -a "$LOG_FILE" || true
    fi
    if command -v numactl >/dev/null 2>&1; then
        log "Final NUMA configuration:"
        numactl --hardware 2>&1 | tee -a "$LOG_FILE" || true
    fi
    log "Guest CXL setup completed"
}

main "$@"
