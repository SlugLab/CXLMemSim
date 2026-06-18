# SSD Streaming Two-QEMU Benchmark Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and run a repeatable end-to-end benchmark where two QEMU CXL Type 3 persistent-memory guests stream through `/dev/dax0.0` while CXLMemSim serves memory from `/mnt/disk0/cxlmemsim-ssd-bench.swap`.

**Architecture:** Fix the QEMU Type 3 first-use forwarding gate so TCP/SSD-backed CXLMemSim is actually authoritative. Add a small DAX stream worker for deterministic write/verify timing, then add a host harness that starts the SSD-backed server, boots two isolated guests, configures guest CXL pmem as devdax, runs bidirectional streaming, and writes artifacts.

**Tech Stack:** Bash, C11, QEMU CXL Type 3, CXLMemSim TCP server, Linux CXL tools, `ndctl`, `daxctl`, SSH/SCP, CMake/Ninja.

---

## File Structure

- Modify: `lib/qemu/hw/mem/cxl_type3.c`
  - Responsibility: make Type 3 TCP/RDMA/SHM forwarding call `cxl_memsim_request_ext()` on first access so the existing lazy connection path runs.
- Modify: `qemu_integration/launch_qemu_cxl_usernet.sh`
  - Responsibility: keep the existing Type 3 launcher, but allow per-VM disk format and MAC address overrides for two simultaneous local guests.
- Create: `qemu_integration/dax_stream_bench.c`
  - Responsibility: deterministic guest-side streaming write/verify benchmark for a DAX character device or a regular mmap-able file used in host tests.
- Create: `qemu_integration/ssd_stream_two_qemu_bench.sh`
  - Responsibility: host orchestration for server startup, QEMU startup, guest CXL setup, worker deployment, bidirectional benchmark, and artifact collection.
- No changes: `qemu_integration/restart_vms_shared.sh`
  - Reason: it references a different checkout and a missing `launch_qemu_cxl1.sh`, so the new benchmark should not build on it.

---

### Task 1: Fix QEMU Type 3 First-Use CXLMemSim Forwarding

**Files:**
- Modify: `lib/qemu/hw/mem/cxl_type3.c`
- Test: local source diff plus QEMU rebuild in Task 4

- [ ] **Step 1: Write the expected source check before editing**

Run:

```bash
rg -n "if \\(g_memsim\\.enabled && g_memsim\\.connected\\)" lib/qemu/hw/mem/cxl_type3.c
```

Expected before the edit: four matches in `get_lsa()`, `set_lsa()`, `cxl_type3_read()`, and `cxl_type3_write()`.

- [ ] **Step 2: Change read/write and LSA forwarding gates**

In `lib/qemu/hw/mem/cxl_type3.c`, replace these four gates:

```c
if (g_memsim.enabled && g_memsim.connected &&
    (g_memsim.transport_mode == CXL_TRANSPORT_TCP ||
     g_memsim.transport_mode == CXL_TRANSPORT_RDMA)) {
```

```c
if (g_memsim.enabled && g_memsim.connected) {
```

with these forms:

```c
if (g_memsim.enabled &&
    (g_memsim.transport_mode == CXL_TRANSPORT_TCP ||
     g_memsim.transport_mode == CXL_TRANSPORT_RDMA)) {
```

```c
if (g_memsim.enabled) {
```

Rationale: `cxl_memsim_request_ext()` already performs the lazy connection with `cxl_memsim_connect_locked()` when `g_memsim.connected` is false. The outer `connected` check prevents the first request from ever attempting the connection.

- [ ] **Step 3: Verify the source gate changed**

Run:

```bash
rg -n "if \\(g_memsim\\.enabled && g_memsim\\.connected\\)" lib/qemu/hw/mem/cxl_type3.c
rg -n "if \\(g_memsim\\.enabled\\)" lib/qemu/hw/mem/cxl_type3.c
```

Expected: the first command exits with no matches; the second command shows the Type 3 forwarding gates.

- [ ] **Step 4: Commit the QEMU source fix inside the submodule**

Run:

```bash
git -C lib/qemu diff -- hw/mem/cxl_type3.c
git -C lib/qemu add hw/mem/cxl_type3.c
git -C lib/qemu commit -m "hw/cxl: connect type3 memsim on first access"
git add lib/qemu
git commit -m "qemu: connect cxl type3 memsim on first access"
```

Expected: the submodule commit includes only `hw/mem/cxl_type3.c`; the top-level commit includes only the `lib/qemu` submodule pointer update. Existing unrelated dirty files inside `lib/qemu` stay unstaged.

---

### Task 2: Make the Existing Usernet Launcher Safe for Two Guests

**Files:**
- Modify: `qemu_integration/launch_qemu_cxl_usernet.sh`
- Test: `bash -n qemu_integration/launch_qemu_cxl_usernet.sh`

- [ ] **Step 1: Add per-VM disk format and MAC variables**

In `qemu_integration/launch_qemu_cxl_usernet.sh`, add these variables after `DISK_IMAGE` and `SSH_PORT`:

```bash
DISK_FORMAT=${DISK_FORMAT:-raw}
NET_MAC=${NET_MAC:-52:54:00:00:10:22}
```

- [ ] **Step 2: Use the variables in the QEMU command**

Replace:

```bash
-drive file="$DISK_IMAGE",index=0,media=disk,format=raw \
```

with:

```bash
-drive file="$DISK_IMAGE",index=0,media=disk,format="$DISK_FORMAT" \
```

Replace:

```bash
-device virtio-net-pci,netdev=net0,mac=52:54:00:00:10:22 \
```

with:

```bash
-device virtio-net-pci,netdev=net0,mac="$NET_MAC" \
```

- [ ] **Step 3: Verify shell syntax**

Run:

```bash
bash -n qemu_integration/launch_qemu_cxl_usernet.sh
```

Expected: exit code `0` with no output.

- [ ] **Step 4: Commit the launcher update**

Run:

```bash
git add qemu_integration/launch_qemu_cxl_usernet.sh
git commit -m "qemu: parameterize cxl usernet launcher"
```

Expected: one commit containing only the launcher update.

---

### Task 3: Add the DAX Stream Worker

**Files:**
- Create: `qemu_integration/dax_stream_bench.c`
- Test: `/tmp/dax_stream_bench_test` against a regular file

- [ ] **Step 1: Create `qemu_integration/dax_stream_bench.c`**

Create the file with this complete source:

```c
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum BenchMode {
    MODE_WRITE,
    MODE_VERIFY,
};

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s --mode write|verify --device PATH --offset BYTES --bytes BYTES --seed N\n",
            argv0);
}

static uint64_t parse_u64(const char *text, const char *name) {
    char *end = NULL;
    errno = 0;
    uint64_t value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return value;
}

static uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static uint64_t pattern_word(uint64_t word_index, uint64_t seed) {
    return splitmix64(word_index ^ (seed * 0x9e3779b97f4a7c15ULL));
}

static double elapsed_sec(struct timespec start, struct timespec end) {
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

int main(int argc, char **argv) {
    const char *device = "/dev/dax0.0";
    enum BenchMode mode = MODE_WRITE;
    int mode_set = 0;
    uint64_t offset = 0;
    uint64_t bytes = 0;
    uint64_t seed = 1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            ++i;
            if (strcmp(argv[i], "write") == 0) {
                mode = MODE_WRITE;
            } else if (strcmp(argv[i], "verify") == 0) {
                mode = MODE_VERIFY;
            } else {
                usage(argv[0]);
                return 2;
            }
            mode_set = 1;
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) {
            offset = parse_u64(argv[++i], "offset");
        } else if (strcmp(argv[i], "--bytes") == 0 && i + 1 < argc) {
            bytes = parse_u64(argv[++i], "bytes");
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = parse_u64(argv[++i], "seed");
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!mode_set || bytes == 0 || (offset % 4096) != 0 || (bytes % sizeof(uint64_t)) != 0) {
        usage(argv[0]);
        return 2;
    }

    int fd = open(device, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", device, strerror(errno));
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "fstat %s failed: %s\n", device, strerror(errno));
        close(fd);
        return 1;
    }

    uint64_t map_len = offset + bytes;
    if (st.st_size > 0 && map_len > (uint64_t)st.st_size) {
        fprintf(stderr, "range exceeds device size: range=%" PRIu64 " size=%" PRIu64 "\n",
                map_len, (uint64_t)st.st_size);
        close(fd);
        return 1;
    }

    void *mapping = mmap(NULL, (size_t)map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "mmap %s failed: %s\n", device, strerror(errno));
        close(fd);
        return 1;
    }

    volatile uint64_t *words = (volatile uint64_t *)((uint8_t *)mapping + offset);
    uint64_t nwords = bytes / sizeof(uint64_t);
    uint64_t errors = 0;
    uint64_t checksum = 0;
    struct timespec start;
    struct timespec end;

    clock_gettime(CLOCK_MONOTONIC, &start);
    if (mode == MODE_WRITE) {
        for (uint64_t i = 0; i < nwords; ++i) {
            uint64_t value = pattern_word(i, seed);
            words[i] = value;
            checksum ^= value;
        }
        if (msync((uint8_t *)mapping + offset, (size_t)bytes, MS_SYNC) != 0) {
            fprintf(stderr, "msync failed: %s\n", strerror(errno));
            munmap(mapping, (size_t)map_len);
            close(fd);
            return 1;
        }
    } else {
        for (uint64_t i = 0; i < nwords; ++i) {
            uint64_t expected = pattern_word(i, seed);
            uint64_t actual = words[i];
            checksum ^= actual;
            if (actual != expected) {
                ++errors;
                if (errors <= 8) {
                    fprintf(stderr,
                            "mismatch word=%" PRIu64 " expected=0x%016" PRIx64 " actual=0x%016" PRIx64 "\n",
                            i, expected, actual);
                }
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double seconds = elapsed_sec(start, end);
    double mib = (double)bytes / (1024.0 * 1024.0);
    double mib_per_sec = seconds > 0.0 ? mib / seconds : 0.0;

    printf("{\"mode\":\"%s\",\"device\":\"%s\",\"offset\":%" PRIu64
           ",\"bytes\":%" PRIu64 ",\"seed\":%" PRIu64
           ",\"elapsed_sec\":%.9f,\"mib_per_sec\":%.3f,\"errors\":%" PRIu64
           ",\"checksum\":\"0x%016" PRIx64 "\"}\n",
           mode == MODE_WRITE ? "write" : "verify",
           device, offset, bytes, seed, seconds, mib_per_sec, errors, checksum);

    munmap(mapping, (size_t)map_len);
    close(fd);
    return errors == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Build the worker with warnings as errors**

Run:

```bash
cc -O2 -Wall -Wextra -Werror -std=c11 -o /tmp/dax_stream_bench_test qemu_integration/dax_stream_bench.c
```

Expected: exit code `0` with no compiler diagnostics.

- [ ] **Step 3: Test write and verify against a regular file**

Run:

```bash
truncate -s 8M /tmp/dax_stream_bench_test.bin
/tmp/dax_stream_bench_test --mode write --device /tmp/dax_stream_bench_test.bin --offset 0 --bytes 4194304 --seed 1001
/tmp/dax_stream_bench_test --mode verify --device /tmp/dax_stream_bench_test.bin --offset 0 --bytes 4194304 --seed 1001
```

Expected: both worker invocations print JSON with `"errors":0`.

- [ ] **Step 4: Test verify failure on the wrong seed**

Run:

```bash
if /tmp/dax_stream_bench_test --mode verify --device /tmp/dax_stream_bench_test.bin --offset 0 --bytes 4194304 --seed 2002; then
  echo "unexpected verify success" >&2
  exit 1
fi
```

Expected: exit code `0` from the shell block because the worker rejects the wrong seed.

- [ ] **Step 5: Commit the worker**

Run:

```bash
git add qemu_integration/dax_stream_bench.c
git commit -m "bench: add dax stream worker"
```

Expected: one commit containing only `qemu_integration/dax_stream_bench.c`.

---

### Task 4: Add the Two-QEMU SSD Streaming Harness

**Files:**
- Create: `qemu_integration/ssd_stream_two_qemu_bench.sh`
- Test: shell syntax check, worker build, server build, QEMU rebuild, then live run

- [ ] **Step 1: Create the host harness script**

Create `qemu_integration/ssd_stream_two_qemu_bench.sh` with this complete source:

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/victoryang00/CXLMemSim}
SERVER_BINARY=${SERVER_BINARY:-$ROOT/build/cxlmemsim_server}
QEMU_BINARY=${QEMU_BINARY:-$ROOT/lib/qemu/build/qemu-system-x86_64}
QEMU_LAUNCHER=${QEMU_LAUNCHER:-$ROOT/qemu_integration/launch_qemu_cxl_usernet.sh}
KERNEL=${KERNEL:-$ROOT/build/bzImage}
BASE_DISK=${BASE_DISK:-$ROOT/build/qemu.img}
BACKING_FILE=${BACKING_FILE:-/mnt/disk0/cxlmemsim-ssd-bench.swap}
RUN_ROOT=${RUN_ROOT:-$ROOT/artifact/ssd_stream_bench}
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

timestamp=$(date +%Y%m%d-%H%M%S)
RUN_DIR=${RUN_DIR:-$RUN_ROOT/$timestamp}
mkdir -p "$RUN_DIR"

SERVER_PID=""
QEMU0_PID=""
QEMU1_PID=""

log() {
    printf '[%s] %s\n' "$(date '+%F %T')" "$*" | tee -a "$RUN_DIR/harness.log"
}

die() {
    log "ERROR: $*"
    exit 1
}

cleanup() {
    local status=$?
    if [[ "$KEEP_RUNNING" == "1" ]]; then
        log "KEEP_RUNNING=1; leaving processes running"
        exit "$status"
    fi
    for pid in "$QEMU0_PID" "$QEMU1_PID" "$SERVER_PID"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    exit "$status"
}
trap cleanup EXIT

require_file() {
    [[ -e "$1" ]] || die "missing required file: $1"
}

wait_tcp() {
    local host=$1
    local port=$2
    local label=$3
    for _ in $(seq 1 120); do
        if timeout 1 bash -c "</dev/tcp/$host/$port" >/dev/null 2>&1; then
            log "$label is reachable at $host:$port"
            return 0
        fi
        sleep 1
    done
    die "$label did not become reachable at $host:$port"
}

ssh_guest() {
    local port=$1
    shift
    ssh -p "$port" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=5 \
        root@127.0.0.1 "$@"
}

scp_to_guest() {
    local port=$1
    local src=$2
    local dst=$3
    scp -P "$port" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=10 \
        "$src" "root@127.0.0.1:$dst"
}

wait_ssh() {
    local port=$1
    local label=$2
    for _ in $(seq 1 240); do
        if ssh_guest "$port" "true" >/dev/null 2>&1; then
            log "$label SSH is ready on port $port"
            return 0
        fi
        sleep 2
    done
    die "$label SSH did not become ready on port $port"
}

copy_disk() {
    local node=$1
    local dst="$RUN_DIR/qemu-node${node}.img"
    if [[ ! -e "$dst" ]]; then
        log "Creating node $node disk image: $dst"
        cp --reflink=auto --sparse=always "$BASE_DISK" "$dst"
    fi
    printf '%s\n' "$dst"
}

start_server() {
    log "Starting CXLMemSim server with SSD streaming backing $BACKING_FILE"
    SPDLOG_LEVEL=${SPDLOG_LEVEL:-info} "$SERVER_BINARY" \
        --comm-mode=tcp \
        --port="$SERVER_PORT" \
        --capacity="$CAPACITY_MB" \
        --default_latency=100 \
        --topology="$ROOT/qemu_integration/topology_simple.txt" \
        --ssd-backing-file="$BACKING_FILE" \
        --ssd-cache-mb="$SSD_CACHE_MB" \
        --ssd-page-size="$SSD_PAGE_SIZE" \
        --ssd-io-chunk-size="$SSD_IO_CHUNK_SIZE" \
        --ssd-read-ahead-pages="$SSD_READ_AHEAD_PAGES" \
        --ssd-io-uring=true \
        --ssd-odirect=true \
        >"$RUN_DIR/cxlmemsim-server.log" 2>&1 &
    SERVER_PID=$!
    printf '%s\n' "$SERVER_PID" >"$RUN_DIR/cxlmemsim-server.pid"
    wait_tcp 127.0.0.1 "$SERVER_PORT" "CXLMemSim server"
}

start_qemu() {
    local node=$1
    local ssh_port=$2
    local mac=$3
    local disk
    disk=$(copy_disk "$node")
    log "Starting QEMU node $node on SSH port $ssh_port"
    env \
        ROOT="$ROOT" \
        QEMU_BINARY="$QEMU_BINARY" \
        KERNEL="$KERNEL" \
        DISK_IMAGE="$disk" \
        DISK_FORMAT=raw \
        SSH_PORT="$ssh_port" \
        NET_MAC="$mac" \
        VM_MEMORY="$VM_MEMORY" \
        VM_MAX_MEMORY="$VM_MAX_MEMORY" \
        SMP="$SMP" \
        CXL_HOST_ID="$node" \
        CXL_TRANSPORT_MODE=tcp \
        CXL_MEMSIM_HOST=127.0.0.1 \
        CXL_MEMSIM_PORT="$SERVER_PORT" \
        CXL_BACKING="$RUN_DIR/cxl-node${node}.raw" \
        CXL_LSA="$RUN_DIR/lsa-node${node}.raw" \
        CXL_BACKING_SIZE="$CXL_BACKING_SIZE" \
        CXL_LSA_SIZE="$CXL_LSA_SIZE" \
        CXL_FMW_SIZE="$CXL_FMW_SIZE" \
        "$QEMU_LAUNCHER" >"$RUN_DIR/qemu-node${node}.log" 2>&1 &
    local pid=$!
    printf '%s\n' "$pid" >"$RUN_DIR/qemu-node${node}.pid"
    if [[ "$node" == "0" ]]; then
        QEMU0_PID=$pid
    else
        QEMU1_PID=$pid
    fi
}

setup_guest_cxl() {
    local port=$1
    local label=$2
    log "Deploying guest CXL setup to $label"
    scp_to_guest "$port" "$ROOT/qemu_integration/setup_cxl_numa.sh" /tmp/setup_cxl_numa.sh
    ssh_guest "$port" "chmod +x /tmp/setup_cxl_numa.sh"
    ssh_guest "$port" \
        "LOG_FILE=/tmp/cxl_numa_setup.log REGION_SIZE=${CAPACITY_MB}M CXL_REGION_TYPE=pmem CXL_CREATE_DAX=1 CXL_DAX_MODE=devdax CXL_CREATE_NDCTL_NAMESPACE=1 CXL_NDCTL_NAMESPACE_MODE=devdax CXL_CONFIGURE_NET=0 /tmp/setup_cxl_numa.sh" \
        >"$RUN_DIR/${label}-setup.out" 2>"$RUN_DIR/${label}-setup.err"
    scp -P "$port" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        "root@127.0.0.1:/tmp/cxl_numa_setup.log" "$RUN_DIR/${label}-cxl_numa_setup.log" >/dev/null 2>&1 || true
    ssh_guest "$port" "test -c /dev/dax0.0" || die "$label did not expose /dev/dax0.0"
}

build_worker() {
    log "Building DAX stream worker"
    cc -O2 -Wall -Wextra -Werror -std=c11 \
        -o "$RUN_DIR/dax_stream_bench" \
        "$ROOT/qemu_integration/dax_stream_bench.c"
}

deploy_worker() {
    local port=$1
    local label=$2
    scp_to_guest "$port" "$RUN_DIR/dax_stream_bench" /tmp/dax_stream_bench
    ssh_guest "$port" "chmod +x /tmp/dax_stream_bench"
    log "Deployed worker to $label"
}

run_worker() {
    local port=$1
    local label=$2
    local mode=$3
    local seed=$4
    local bytes=$((TRANSFER_MB * 1024 * 1024))
    log "Running $label $mode seed=$seed bytes=$bytes"
    ssh_guest "$port" "/tmp/dax_stream_bench --mode $mode --device /dev/dax0.0 --offset 0 --bytes $bytes --seed $seed" \
        | tee -a "$RUN_DIR/results.jsonl"
}

write_summary() {
    {
        echo "run_dir=$RUN_DIR"
        echo "backing_file=$BACKING_FILE"
        echo "transfer_mb=$TRANSFER_MB"
        echo "capacity_mb=$CAPACITY_MB"
        echo "server_port=$SERVER_PORT"
        echo "qemu_binary=$QEMU_BINARY"
        echo "node0_ssh_port=$SSH_PORT0"
        echo "node1_ssh_port=$SSH_PORT1"
    } >"$RUN_DIR/config.txt"
}

main() {
    require_file "$SERVER_BINARY"
    require_file "$QEMU_BINARY"
    require_file "$QEMU_LAUNCHER"
    require_file "$KERNEL"
    require_file "$BASE_DISK"
    require_file "$ROOT/qemu_integration/setup_cxl_numa.sh"
    require_file "$ROOT/qemu_integration/dax_stream_bench.c"
    write_summary
    build_worker
    start_server
    start_qemu 0 "$SSH_PORT0" 52:54:00:00:10:20
    start_qemu 1 "$SSH_PORT1" 52:54:00:00:10:21
    wait_ssh "$SSH_PORT0" node0
    wait_ssh "$SSH_PORT1" node1
    setup_guest_cxl "$SSH_PORT0" node0
    setup_guest_cxl "$SSH_PORT1" node1
    deploy_worker "$SSH_PORT0" node0
    deploy_worker "$SSH_PORT1" node1
    : >"$RUN_DIR/results.jsonl"
    run_worker "$SSH_PORT0" node0 write 1001
    run_worker "$SSH_PORT1" node1 verify 1001
    run_worker "$SSH_PORT1" node1 write 2002
    run_worker "$SSH_PORT0" node0 verify 2002
    log "Benchmark complete; artifacts: $RUN_DIR"
}

main "$@"
```

- [ ] **Step 2: Make the harness executable**

Run:

```bash
chmod +x qemu_integration/ssd_stream_two_qemu_bench.sh
```

Expected: exit code `0`.

- [ ] **Step 3: Verify shell syntax**

Run:

```bash
bash -n qemu_integration/ssd_stream_two_qemu_bench.sh
```

Expected: exit code `0` with no output.

- [ ] **Step 4: Re-run the worker tests**

Run:

```bash
cc -O2 -Wall -Wextra -Werror -std=c11 -o /tmp/dax_stream_bench_test qemu_integration/dax_stream_bench.c
truncate -s 8M /tmp/dax_stream_bench_test.bin
/tmp/dax_stream_bench_test --mode write --device /tmp/dax_stream_bench_test.bin --offset 0 --bytes 4194304 --seed 1001
/tmp/dax_stream_bench_test --mode verify --device /tmp/dax_stream_bench_test.bin --offset 0 --bytes 4194304 --seed 1001
```

Expected: compiler success and two JSON lines with `"errors":0`.

- [ ] **Step 5: Build the CXLMemSim server**

Run:

```bash
cmake --build build -j --target cxlmemsim_server
```

Expected: build completes and leaves `build/cxlmemsim_server` executable.

- [ ] **Step 6: Rebuild the local QEMU binary**

Run:

```bash
JOBS=8 ./script/build_qemu.sh
```

Expected: build completes and leaves `lib/qemu/build/qemu-system-x86_64` executable.

- [ ] **Step 7: Commit the harness**

Run:

```bash
git add qemu_integration/ssd_stream_two_qemu_bench.sh
git commit -m "bench: add two-qemu ssd streaming harness"
```

Expected: one commit containing only the harness script.

---

### Task 5: Run the End-to-End Benchmark

**Files:**
- Read: `artifact/ssd_stream_bench/YYYYmmdd-HHMMSS/results.jsonl`
- Read: `artifact/ssd_stream_bench/YYYYmmdd-HHMMSS/cxlmemsim-server.log`
- Read: `artifact/ssd_stream_bench/YYYYmmdd-HHMMSS/qemu-node0.log`
- Read: `artifact/ssd_stream_bench/YYYYmmdd-HHMMSS/qemu-node1.log`

- [ ] **Step 1: Confirm no conflicting processes are running**

Run:

```bash
pgrep -af 'qemu-system|cxlmemsim_server' || true
```

Expected: no existing QEMU or `cxlmemsim_server` processes that use ports `11022`, `11023`, or `19999`.

- [ ] **Step 2: Run the bounded benchmark**

Run:

```bash
TRANSFER_MB=256 \
BACKING_FILE=/mnt/disk0/cxlmemsim-ssd-bench.swap \
qemu_integration/ssd_stream_two_qemu_bench.sh
```

Expected: the script exits `0`, prints the artifact directory, and writes four JSON records to `results.jsonl`: node0 write, node1 verify, node1 write, node0 verify.

- [ ] **Step 3: Verify result records**

Run:

```bash
latest=$(ls -td artifact/ssd_stream_bench/* | head -1)
cat "$latest/results.jsonl"
rg -n '"errors":0' "$latest/results.jsonl"
```

Expected: `cat` shows four JSON records, and `rg` shows four matches.

- [ ] **Step 4: Verify server SSD streaming was active**

Run:

```bash
latest=$(ls -td artifact/ssd_stream_bench/* | head -1)
rg -n "SSD streaming|SSD Backing|page_faults|READ|WRITE|Total operations" "$latest/cxlmemsim-server.log"
```

Expected: the log includes SSD streaming initialization for `/mnt/disk0/cxlmemsim-ssd-bench.swap` and read/write operation statistics.

- [ ] **Step 5: Report the benchmark**

Summarize:

```text
Artifact directory:
Server backing file:
Transfer size:
Node0 write MiB/s:
Node1 verify MiB/s:
Node1 write MiB/s:
Node0 verify MiB/s:
Verification status:
Server/QEMU caveats:
```

Expected: the summary references the exact artifact directory and the four measured `mib_per_sec` values from `results.jsonl`.

---

## Self-Review Checklist

- Spec coverage: the plan starts the SSD-backed server, boots two QEMU guests, configures `/dev/dax0.0`, runs bidirectional streaming, and stores artifacts.
- Error handling: the harness fails on missing files, server readiness failure, SSH failure, guest DAX absence, and worker verification failure.
- Isolation: the launcher uses separate SSH ports, MAC addresses, disk images, CXL local backing files, and LSA files per guest.
- False-positive prevention: separate local CXL backing files mean cross-guest verification only passes if the Type 3 TCP CXLMemSim path is active.
- Testing: the worker has positive and negative host tests; scripts have syntax checks; server and QEMU have build gates before the live run.
