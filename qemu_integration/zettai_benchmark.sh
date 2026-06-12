#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

RUN_DIR=${ZETTAI_BENCH_RUN_DIR:-"$REPO_ROOT/build/zettai-bench"}
QMP_SOCKET=${ZETTAI_QMP_SOCKET:-/tmp/zettai-qmp.sock}
SERVER_HOST=${CXL_MEMSIM_HOST:-127.0.0.1}
SERVER_PORT=${CXL_MEMSIM_PORT:-9999}
LAUNCH_QEMU=0
RUN_GUEST=0
RUN_TYPE2_BENCH=${ZETTAI_RUN_TYPE2_BENCH:-0}
KEEP_QEMU=${ZETTAI_KEEP_QEMU:-0}
GUEST_SSH=${ZETTAI_GUEST_SSH:-}
GUEST_DIR=${ZETTAI_GUEST_DIR:-/root/CXLMemSim/qemu_integration}
QEMU_PID=

usage() {
    cat <<'EOF'
Usage: zettai_benchmark.sh [--launch] [--guest] [--run-type2-bench] [--keep-qemu]

Host-side Zettai DCD/GFAM benchmark harness.

Default behavior assumes QEMU is already running with:
  QEMU_EXTRA_ARGS="-qmp unix:/tmp/zettai-qmp.sock,server=on,wait=off"

Options:
  --launch           Start launch_qemu_vcs_dcd_gfam.sh in the background.
  --guest            Run guest-side DCD/GFAM setup through ZETTAI_GUEST_SSH.
  --run-type2-bench  Compile and run guest_libcuda/cxl_bar_benchmark in guest.
  --keep-qemu        Do not terminate QEMU when this script launched it.
  --help             Show this help.

Environment:
  ZETTAI_QMP_SOCKET       QMP socket path, default: /tmp/zettai-qmp.sock
  ZETTAI_GUEST_SSH        SSH command for guest, e.g. "ssh root@192.168.122.10"
  ZETTAI_GUEST_DIR        Guest qemu_integration path, default: /root/CXLMemSim/qemu_integration
  ZETTAI_BENCH_RUN_DIR    Host output directory, default: build/zettai-bench
  CXL_MEMSIM_HOST/PORT    CXLMemSim TCP endpoint, default: 127.0.0.1:9999
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --launch)
            LAUNCH_QEMU=1
            ;;
        --guest)
            RUN_GUEST=1
            ;;
        --run-type2-bench)
            RUN_TYPE2_BENCH=1
            RUN_GUEST=1
            ;;
        --keep-qemu)
            KEEP_QEMU=1
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
    shift
done

mkdir -p "$RUN_DIR"

cleanup() {
    if [[ -n "${QEMU_PID:-}" && "$KEEP_QEMU" != 1 ]]; then
        kill "$QEMU_PID" >/dev/null 2>&1 || true
        wait "$QEMU_PID" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

wait_for_qmp() {
    for _ in $(seq 1 200); do
        if [[ -S "$QMP_SOCKET" ]]; then
            return 0
        fi
        sleep 0.1
    done

    echo "Timed out waiting for QMP socket: $QMP_SOCKET" >&2
    return 1
}

launch_qemu() {
    echo "Launching QEMU; log: $RUN_DIR/qemu.log"
    rm -f "$QMP_SOCKET"

    QEMU_NET_MODE=${QEMU_NET_MODE:-none} \
    QEMU_EXTRA_ARGS="${QEMU_EXTRA_ARGS:-} -qmp unix:$QMP_SOCKET,server=on,wait=off" \
        "$SCRIPT_DIR/launch_qemu_vcs_dcd_gfam.sh" \
        >"$RUN_DIR/qemu.log" 2>&1 &
    QEMU_PID=$!

    wait_for_qmp
}

run_host_flow() {
    local log=$1

    echo "Running host Zettai bind/add/query flow"
    python3 "$SCRIPT_DIR/zettai_host_dcd_gfam_test.py" \
        --qmp "$QMP_SOCKET" \
        --server-host "$SERVER_HOST" \
        --server-port "$SERVER_PORT" \
        --bind --add --query --ignore-bind-error \
        2>&1 | tee "$log"
}

run_guest_command() {
    local command=$1

    if [[ -z "$GUEST_SSH" ]]; then
        echo "ZETTAI_GUEST_SSH is not set; skipping guest command:" >&2
        echo "  $command" >&2
        return 0
    fi

    read -r -a guest_ssh_argv <<< "$GUEST_SSH"
    "${guest_ssh_argv[@]}" "$command"
}

run_guest_flow() {
    local log=$1

    echo "Running guest DCD/GFAM setup"
    run_guest_command "cd '$GUEST_DIR' && sudo ./zettai_guest_dcd_gfam_test.sh" \
        2>&1 | tee "$log"
}

run_type2_benchmark() {
    local log=$1

    echo "Running guest Type2 fabric-memory BAR benchmark"
    run_guest_command "cd '$GUEST_DIR/guest_libcuda' && gcc -O2 -Wall -o cxl_bar_benchmark cxl_bar_benchmark.c -lrt -lpthread && sudo ./cxl_bar_benchmark" \
        2>&1 | tee "$log"
}

if [[ "$LAUNCH_QEMU" == 1 ]]; then
    launch_qemu
else
    wait_for_qmp
fi

run_host_flow "$RUN_DIR/host-before-guest.log"

if [[ "$RUN_GUEST" == 1 ]]; then
    run_guest_flow "$RUN_DIR/guest-dcd-gfam.log"
fi

if [[ "$RUN_TYPE2_BENCH" == 1 ]]; then
    run_type2_benchmark "$RUN_DIR/guest-type2-bar-benchmark.log"
fi

echo "Querying CXLMemSim counters after guest activity"
python3 "$SCRIPT_DIR/zettai_host_dcd_gfam_test.py" \
    --server-host "$SERVER_HOST" \
    --server-port "$SERVER_PORT" \
    --query 2>&1 | tee "$RUN_DIR/host-after-guest.log"

echo "Zettai benchmark logs written to $RUN_DIR"
