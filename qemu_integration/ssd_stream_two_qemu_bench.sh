#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/victoryang00/CXLMemSim}
SERVER_BINARY=${SERVER_BINARY:-$ROOT/build/cxlmemsim_server}
QEMU_BINARY=${QEMU_BINARY:-$ROOT/lib/qemu/build/qemu-system-x86_64}
QEMU_LAUNCHER=${QEMU_LAUNCHER:-$ROOT/qemu_integration/launch_qemu_cxl_usernet.sh}
KERNEL=${KERNEL:-$ROOT/build/bzImage}
BASE_DISK=${BASE_DISK:-$ROOT/build/qemu.img}
BACKING_FILE=${BACKING_FILE:-/mnt/disk0/cxlmemsim-ssd-bench.swap}
RUN_ROOT=${RUN_ROOT:-/mnt/disk0/cxlmemsim-ssd-stream-bench-artifact}
TRANSFER_MB=${TRANSFER_MB:-256}
CAPACITY_MB=${CAPACITY_MB:-1024}
SERVER_PORT=${SERVER_PORT:-19999}
SSD_CACHE_MB=${SSD_CACHE_MB:-16}
SSD_PAGE_SIZE=${SSD_PAGE_SIZE:-4096}
SSD_IO_CHUNK_SIZE=${SSD_IO_CHUNK_SIZE:-65536}
SSD_READ_AHEAD_PAGES=${SSD_READ_AHEAD_PAGES:-16}
SSH_PORT0=${SSH_PORT0:-11022}
SSH_PORT1=${SSH_PORT1:-11023}
VM_MEMORY=${VM_MEMORY:-8G}
VM_MAX_MEMORY=${VM_MAX_MEMORY:-16G}
SMP=${SMP:-4}
CXL_BACKING_SIZE=${CXL_BACKING_SIZE:-${CAPACITY_MB}M}
CXL_LSA_SIZE=${CXL_LSA_SIZE:-256M}
CXL_FMW_SIZE=${CXL_FMW_SIZE:-4G}
KEEP_RUNNING=${KEEP_RUNNING:-0}
SPDLOG_LEVEL=${SPDLOG_LEVEL:-info}

SETUP_HELPER=$ROOT/qemu_integration/setup_cxl_numa.sh
DAX_WORKER_SOURCE=$ROOT/qemu_integration/dax_stream_bench.c
TOPOLOGY_FILE=$ROOT/qemu_integration/topology_simple.txt
TIMESTAMP=$(date -u '+%Y%m%dT%H%M%SZ')
RUN_DIR=$RUN_ROOT/$TIMESTAMP
HARNESS_LOG=$RUN_DIR/harness.log
CONFIG_FILE=$RUN_DIR/config.txt
RESULTS_FILE=$RUN_DIR/results.jsonl
SERVER_PID_FILE=$RUN_DIR/cxlmemsim-server.pid

SSH_USER=root
SSH_HOST=127.0.0.1
SSH_OPTS=(
    -o BatchMode=yes
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile=/dev/null
    -o ConnectTimeout=5
    -o ServerAliveInterval=5
    -o ServerAliveCountMax=2
    -o LogLevel=ERROR
)

SERVER_PID=""
QEMU_PID0=""
QEMU_PID1=""

log() {
    local message=$*

    printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$message" | tee -a "$HARNESS_LOG" >&2
}

die() {
    log "ERROR: $*"
    exit 1
}

cleanup() {
    local status=$?

    if [[ "$KEEP_RUNNING" == "1" ]]; then
        log "KEEP_RUNNING=1; leaving server and QEMU processes running"
        return "$status"
    fi

    if [[ -n "$QEMU_PID0" ]] && kill -0 "$QEMU_PID0" 2>/dev/null; then
        log "Stopping node0 QEMU pid $QEMU_PID0"
        kill "$QEMU_PID0" 2>/dev/null || true
        wait "$QEMU_PID0" 2>/dev/null || true
    fi
    if [[ -n "$QEMU_PID1" ]] && kill -0 "$QEMU_PID1" 2>/dev/null; then
        log "Stopping node1 QEMU pid $QEMU_PID1"
        kill "$QEMU_PID1" 2>/dev/null || true
        wait "$QEMU_PID1" 2>/dev/null || true
    fi
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        log "Stopping cxlmemsim server pid $SERVER_PID"
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi

    return "$status"
}

require_file() {
    local path=$1
    local label=$2

    [[ -e "$path" ]] || die "Missing required $label: $path"
}

assert_process_alive() {
    local pid=$1
    local label=$2

    [[ -n "$pid" ]] || die "$label pid is not set"
    kill -0 "$pid" 2>/dev/null || die "$label process pid $pid is not running"
}

assert_port_free() {
    local host=$1
    local port=$2
    local label=$3

    if (echo >"/dev/tcp/$host/$port") >/dev/null 2>&1; then
        die "$label port $host:$port is already in use"
    fi
}

preflight_ports() {
    if [[ "$SERVER_PORT" == "$SSH_PORT0" || "$SERVER_PORT" == "$SSH_PORT1" || "$SSH_PORT0" == "$SSH_PORT1" ]]; then
        die "SERVER_PORT, SSH_PORT0, and SSH_PORT1 must be distinct"
    fi

    assert_port_free 127.0.0.1 "$SERVER_PORT" "server"
    assert_port_free 127.0.0.1 "$SSH_PORT0" "node0 SSH forward"
    assert_port_free 127.0.0.1 "$SSH_PORT1" "node1 SSH forward"
}

wait_tcp() {
    local host=$1
    local port=$2
    local label=$3
    local pid=$4
    local timeout_sec=${5:-60}
    local start now

    start=$(date +%s)
    while true; do
        assert_process_alive "$pid" "$label"
        if (echo >"/dev/tcp/$host/$port") >/dev/null 2>&1; then
            assert_process_alive "$pid" "$label"
            log "$label is accepting TCP on $host:$port"
            return 0
        fi

        now=$(date +%s)
        if ((now - start >= timeout_sec)); then
            die "Timed out waiting for $label on $host:$port"
        fi
        sleep 1
    done
}

ssh_guest() {
    local port=$1

    shift
    ssh "${SSH_OPTS[@]}" -p "$port" "$SSH_USER@$SSH_HOST" "$@"
}

scp_to_guest() {
    local port=$1
    local src=$2
    local dst=$3

    scp "${SSH_OPTS[@]}" -P "$port" "$src" "$SSH_USER@$SSH_HOST:$dst"
}

wait_ssh() {
    local node=$1
    local port=$2
    local timeout_sec=${3:-180}
    local pid
    local start now

    if [[ "$node" == "0" ]]; then
        pid=$QEMU_PID0
    else
        pid=$QEMU_PID1
    fi

    start=$(date +%s)
    while true; do
        assert_process_alive "$pid" "node$node QEMU"
        if ssh_guest "$port" true >/dev/null 2>&1; then
            assert_process_alive "$pid" "node$node QEMU"
            log "node$node SSH is ready on port $port"
            return 0
        fi

        now=$(date +%s)
        if ((now - start >= timeout_sec)); then
            die "Timed out waiting for node$node SSH on port $port"
        fi
        sleep 2
    done
}

copy_disk() {
    local node=$1
    local dst=$2

    log "Copying base disk for node$node to $dst"
    cp --reflink=auto "$BASE_DISK" "$dst" || die "Failed to copy base disk for node$node to $dst"
    [[ -s "$dst" ]] || die "Copied disk for node$node is missing or empty: $dst"
}

start_server() {
    local log_file=$RUN_DIR/cxlmemsim-server.log

    log "Starting cxlmemsim server on port $SERVER_PORT"
    SPDLOG_LEVEL=$SPDLOG_LEVEL "$SERVER_BINARY" \
        --comm-mode=tcp \
        --port="$SERVER_PORT" \
        --capacity="$CAPACITY_MB" \
        --default_latency=100 \
        --topology="$TOPOLOGY_FILE" \
        --ssd-backing-file="$BACKING_FILE" \
        --ssd-cache-mb="$SSD_CACHE_MB" \
        --ssd-page-size="$SSD_PAGE_SIZE" \
        --ssd-io-chunk-size="$SSD_IO_CHUNK_SIZE" \
        --ssd-read-ahead-pages="$SSD_READ_AHEAD_PAGES" \
        --ssd-io-uring=true \
        --ssd-odirect=true \
        >"$log_file" 2>&1 &
    SERVER_PID=$!
    printf '%s\n' "$SERVER_PID" >"$SERVER_PID_FILE"

    wait_tcp 127.0.0.1 "$SERVER_PORT" "cxlmemsim server" "$SERVER_PID" 90
}

start_qemu() {
    local node=$1
    local ssh_port=$2
    local mac=$3
    local disk_image=$4
    local qemu_log=$RUN_DIR/qemu-node${node}.log
    local pid_file=$RUN_DIR/qemu-node${node}.pid

    log "Starting node$node QEMU on SSH port $ssh_port"
    env \
        ROOT="$ROOT" \
        QEMU_BINARY="$QEMU_BINARY" \
        KERNEL="$KERNEL" \
        DISK_IMAGE="$disk_image" \
        DISK_FORMAT=raw \
        SSH_PORT="$ssh_port" \
        NET_MAC="$mac" \
        VM_MEMORY="$VM_MEMORY" \
        VM_MAX_MEMORY="$VM_MAX_MEMORY" \
        SMP="$SMP" \
        CXL_BACKING="$RUN_DIR/cxl-node${node}.pmem" \
        CXL_LSA="$RUN_DIR/cxl-node${node}.lsa" \
        CXL_BACKING_SIZE="$CXL_BACKING_SIZE" \
        CXL_LSA_SIZE="$CXL_LSA_SIZE" \
        CXL_FMW_SIZE="$CXL_FMW_SIZE" \
        CXL_TRANSPORT_MODE=tcp \
        CXL_HOST_ID="$node" \
        CXL_MEMSIM_HOST=127.0.0.1 \
        CXL_MEMSIM_PORT="$SERVER_PORT" \
        "$QEMU_LAUNCHER" >"$qemu_log" 2>&1 &

    if [[ "$node" == "0" ]]; then
        QEMU_PID0=$!
        printf '%s\n' "$QEMU_PID0" >"$pid_file"
    else
        QEMU_PID1=$!
        printf '%s\n' "$QEMU_PID1" >"$pid_file"
    fi
}

setup_guest_cxl() {
    local node=$1
    local port=$2
    local stdout_log=$RUN_DIR/setup-node${node}.stdout.log
    local stderr_log=$RUN_DIR/setup-node${node}.stderr.log
    local remote_log=$RUN_DIR/setup-node${node}.remote.log

    log "Deploying CXL setup helper to node$node"
    scp_to_guest "$port" "$SETUP_HELPER" /tmp/setup_cxl_numa.sh
    ssh_guest "$port" chmod +x /tmp/setup_cxl_numa.sh

    log "Configuring CXL devdax on node$node"
    if ! ssh_guest "$port" \
        "LOG_FILE=/tmp/cxl_numa_setup.log REGION_SIZE=${CAPACITY_MB}M CXL_REGION_TYPE=pmem CXL_CREATE_DAX=1 CXL_DAX_MODE=devdax CXL_CREATE_NDCTL_NAMESPACE=1 CXL_NDCTL_NAMESPACE_MODE=devdax CXL_CONFIGURE_NET=0 /tmp/setup_cxl_numa.sh" \
        >"$stdout_log" 2>"$stderr_log"; then
        ssh_guest "$port" "cat /tmp/cxl_numa_setup.log" >"$remote_log" 2>/dev/null || true
        die "CXL setup failed on node$node; see $stdout_log, $stderr_log, and $remote_log"
    fi

    ssh_guest "$port" "cat /tmp/cxl_numa_setup.log" >"$remote_log" 2>/dev/null || true
    ssh_guest "$port" "test -c /dev/dax0.0"
    log "node$node has /dev/dax0.0"
}

build_worker() {
    log "Building DAX stream worker into $RUN_DIR/dax_stream_bench"
    cc -O2 -Wall -Wextra -Werror -std=c11 -o "$RUN_DIR/dax_stream_bench" "$DAX_WORKER_SOURCE"
}

deploy_worker() {
    local node=$1
    local port=$2

    log "Deploying DAX stream worker to node$node"
    scp_to_guest "$port" "$RUN_DIR/dax_stream_bench" /tmp/dax_stream_bench
    ssh_guest "$port" chmod +x /tmp/dax_stream_bench
}

run_worker() {
    local node=$1
    local port=$2
    local mode=$3
    local seed=$4
    local bytes=$((TRANSFER_MB * 1024 * 1024))
    local stderr_log=$RUN_DIR/worker-node${node}-${mode}-seed${seed}.stderr.log

    log "Running node$node $mode seed=$seed bytes=$bytes"
    ssh_guest "$port" \
        "/tmp/dax_stream_bench --mode $mode --device /dev/dax0.0 --offset 0 --bytes $bytes --seed $seed" \
        2> >(tee -a "$stderr_log" >&2) | tee -a "$RESULTS_FILE"
}

write_config() {
    cat >"$CONFIG_FILE" <<EOF
ROOT=$ROOT
SERVER_BINARY=$SERVER_BINARY
QEMU_BINARY=$QEMU_BINARY
QEMU_LAUNCHER=$QEMU_LAUNCHER
KERNEL=$KERNEL
BASE_DISK=$BASE_DISK
BACKING_FILE=$BACKING_FILE
RUN_ROOT=$RUN_ROOT
RUN_DIR=$RUN_DIR
TRANSFER_MB=$TRANSFER_MB
CAPACITY_MB=$CAPACITY_MB
SERVER_PORT=$SERVER_PORT
SSD_CACHE_MB=$SSD_CACHE_MB
SSD_PAGE_SIZE=$SSD_PAGE_SIZE
SSD_IO_CHUNK_SIZE=$SSD_IO_CHUNK_SIZE
SSD_READ_AHEAD_PAGES=$SSD_READ_AHEAD_PAGES
SSH_PORT0=$SSH_PORT0
SSH_PORT1=$SSH_PORT1
VM_MEMORY=$VM_MEMORY
VM_MAX_MEMORY=$VM_MAX_MEMORY
SMP=$SMP
CXL_BACKING_SIZE=$CXL_BACKING_SIZE
CXL_LSA_SIZE=$CXL_LSA_SIZE
CXL_FMW_SIZE=$CXL_FMW_SIZE
KEEP_RUNNING=$KEEP_RUNNING
SPDLOG_LEVEL=$SPDLOG_LEVEL
EOF
}

write_summary() {
    local summary_file=$RUN_DIR/summary.txt

    {
        printf 'run_dir=%s\n' "$RUN_DIR"
        printf 'server_pid=%s\n' "$SERVER_PID"
        printf 'qemu_node0_pid=%s\n' "$QEMU_PID0"
        printf 'qemu_node1_pid=%s\n' "$QEMU_PID1"
        printf 'results=%s\n' "$RESULTS_FILE"
        printf 'harness_log=%s\n' "$HARNESS_LOG"
        printf 'server_log=%s\n' "$RUN_DIR/cxlmemsim-server.log"
        printf 'qemu_node0_log=%s\n' "$RUN_DIR/qemu-node0.log"
        printf 'qemu_node1_log=%s\n' "$RUN_DIR/qemu-node1.log"
    } >"$summary_file"
    log "Wrote summary to $summary_file"
}

main() {
    mkdir -p "$RUN_DIR"
    touch "$HARNESS_LOG" "$RESULTS_FILE"
    trap cleanup EXIT

    require_file "$SERVER_BINARY" "server binary"
    require_file "$QEMU_BINARY" "QEMU binary"
    require_file "$QEMU_LAUNCHER" "QEMU launcher"
    require_file "$KERNEL" "kernel"
    require_file "$BASE_DISK" "base disk"
    require_file "$SETUP_HELPER" "guest setup helper"
    require_file "$DAX_WORKER_SOURCE" "DAX worker source"
    require_file "$TOPOLOGY_FILE" "server topology"

    preflight_ports
    write_config
    build_worker

    local disk0 disk1
    disk0=$RUN_DIR/qemu-node0.img
    disk1=$RUN_DIR/qemu-node1.img
    copy_disk 0 "$disk0"
    copy_disk 1 "$disk1"

    start_server
    start_qemu 0 "$SSH_PORT0" 52:54:00:00:10:20 "$disk0"
    start_qemu 1 "$SSH_PORT1" 52:54:00:00:10:21 "$disk1"

    wait_ssh 0 "$SSH_PORT0"
    wait_ssh 1 "$SSH_PORT1"

    setup_guest_cxl 0 "$SSH_PORT0"
    setup_guest_cxl 1 "$SSH_PORT1"

    deploy_worker 0 "$SSH_PORT0"
    deploy_worker 1 "$SSH_PORT1"

    run_worker 0 "$SSH_PORT0" write 1001
    run_worker 1 "$SSH_PORT1" verify 1001
    run_worker 1 "$SSH_PORT1" write 2002
    run_worker 0 "$SSH_PORT0" verify 2002

    write_summary
    log "Final artifact directory: $RUN_DIR"
}

main "$@"
