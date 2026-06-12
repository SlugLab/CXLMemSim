#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

QEMU_BINARY=${QEMU_BINARY:-"$REPO_ROOT/lib/qemu/build/qemu-system-x86_64"}
SERVER_BINARY=${CXL_MEMSIM_SERVER_BINARY:-"$REPO_ROOT/build/cxlmemsim_server"}
RUN_DIR=${CXL_MEMSIM_RUN_DIR:-"$REPO_ROOT/build/type2-smoke"}

CXL_MEMSIM_HOST=${CXL_MEMSIM_HOST:-127.0.0.1}
CXL_MEMSIM_PORT=${CXL_MEMSIM_PORT:-10099}
CXL_CAPACITY_MB=${CXL_CAPACITY_MB:-256}
CXL_DEFAULT_LATENCY=${CXL_DEFAULT_LATENCY:-100}
CXL_TYPE2_MEM_SIZE=${CXL_TYPE2_MEM_SIZE:-64M}
CXL_TYPE2_CACHE_SIZE=${CXL_TYPE2_CACHE_SIZE:-16M}
QEMU_TIMEOUT=${QEMU_TIMEOUT:-5}

SERVER_PID=

server_is_up() {
    timeout 1 bash -c "</dev/tcp/$CXL_MEMSIM_HOST/$CXL_MEMSIM_PORT" >/dev/null 2>&1
}

cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" >/dev/null 2>&1; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

if [[ ! -x "$QEMU_BINARY" ]]; then
    echo "QEMU binary not found or not executable: $QEMU_BINARY" >&2
    exit 1
fi
if [[ ! -x "$SERVER_BINARY" ]]; then
    echo "CXLMemSim server not found or not executable: $SERVER_BINARY" >&2
    exit 1
fi

mkdir -p "$RUN_DIR"
SERVER_LOG="$RUN_DIR/cxlmemsim-server.log"
QEMU_LOG="$RUN_DIR/qemu-type2-smoke.log"

if ! server_is_up; then
    "$SERVER_BINARY" \
        --comm-mode=tcp \
        --port="$CXL_MEMSIM_PORT" \
        --capacity="$CXL_CAPACITY_MB" \
        --default_latency="$CXL_DEFAULT_LATENCY" \
        >"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!

    for _ in $(seq 1 50); do
        if server_is_up; then
            break
        fi
        if ! kill -0 "$SERVER_PID" >/dev/null 2>&1; then
            echo "CXLMemSim server exited during startup." >&2
            tail -100 "$SERVER_LOG" >&2 || true
            exit 1
        fi
        sleep 0.1
    done

    if ! server_is_up; then
        echo "Timed out waiting for CXLMemSim server." >&2
        tail -100 "$SERVER_LOG" >&2 || true
        exit 1
    fi
fi

set +e
env CXL_TRANSPORT_MODE=tcp \
    CXL_MEMSIM_HOST="$CXL_MEMSIM_HOST" \
    CXL_MEMSIM_PORT="$CXL_MEMSIM_PORT" \
    timeout "$QEMU_TIMEOUT" "$QEMU_BINARY" \
        -M q35,cxl=on,cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=1G \
        -m 1G \
        -smp 1 \
        -nodefaults \
        -display none \
        -serial none \
        -monitor none \
        -S \
        -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.0 \
        -device cxl-rp,port=0,bus=cxl.0,id=type2_rp,chassis=0,slot=2 \
        -device "cxl-type2,bus=type2_rp,id=cxl-type2-smoke,sn=200,gpu-mode=0,cache-size=$CXL_TYPE2_CACHE_SIZE,mem-size=$CXL_TYPE2_MEM_SIZE,cxlmemsim-addr=$CXL_MEMSIM_HOST,cxlmemsim-port=$CXL_MEMSIM_PORT,coherency-enabled=true,dcd=on,dcd-granularity=1M,dcd-initial-size=$CXL_TYPE2_MEM_SIZE,gfam=on,gfam-hosts=4,gfam-host-id=0,mhsld=on,mhsld-heads=4,mhsld-head-id=0" \
        >"$QEMU_LOG" 2>&1
QEMU_RC=$?
set -e

if [[ "$QEMU_RC" != 0 && "$QEMU_RC" != 124 ]]; then
    echo "QEMU Type2 smoke failed with rc=$QEMU_RC" >&2
    tail -100 "$QEMU_LOG" >&2 || true
    exit "$QEMU_RC"
fi

if ! grep -q "CXL Type2: Device realized" "$QEMU_LOG"; then
    echo "QEMU log does not show Type2 device realization." >&2
    tail -100 "$QEMU_LOG" >&2 || true
    exit 1
fi

if ! grep -q "Connected to CXLMemSim" "$QEMU_LOG"; then
    echo "QEMU log does not show a CXLMemSim connection." >&2
    tail -100 "$QEMU_LOG" >&2 || true
    exit 1
fi

echo "Type2 endpoint smoke passed"
echo "  QEMU log:   $QEMU_LOG"
echo "  Server log: $SERVER_LOG"
