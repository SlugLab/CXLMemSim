#!/bin/bash
# Splash overnight orchestrator.
# Runs, in sequence:
#   1. RQ1 BFS at multiple N values (long).
#   2. RQ4 alloc-policy at a few N values.
#   3. RQ2 directory-capacity sweep (reboots VM per capacity).
#   4. Multi-GPU scaling sweep (reboots VM per accelerator count).
# After each stage, aggregates CSVs and regenerates the markdown + PDFs so
# whatever finished is usable even if a later stage hangs.
#
# Designed to run under nohup. Logs to artifact/splash_sweep/overnight/.

set -u
REPO=/home/victoryang00/CXLMemSim
ART=${REPO}/artifact/splash_sweep
OVERNIGHT=${ART}/overnight
mkdir -p "${OVERNIGHT}"

PY=${PY:-python3}
SWEEP=${REPO}/script/splash_sweep_driver.py
AGG=${REPO}/script/splash_aggregate.py
REPORT=${REPO}/script/splash_report.py
RQ2=${REPO}/script/splash_rq2_sweep.py
MULTIGPU=${REPO}/script/splash_multigpu_sweep.py
MONITOR_SOCK=/tmp/qemu-mon.sock

LOG_MASTER=${OVERNIGHT}/overnight.log

ts() { date "+%Y-%m-%d %H:%M:%S"; }
log() { echo "[$(ts)] $*" | tee -a "${LOG_MASTER}"; }

wait_ssh() {
    local port=$1
    local probes=${2:-120}
    local i
    for ((i=0; i<probes; i++)); do
        if ssh -o ConnectTimeout=3 -o BatchMode=yes -o StrictHostKeyChecking=no \
               -o UserKnownHostsFile=/dev/null -p "$port" root@127.0.0.1 \
               'echo UP' 2>/dev/null | grep -q UP; then
            return 0
        fi
        sleep 3
    done
    return 1
}

kill_qemu() {
    for p in $(pgrep -f qemu-system-x86_64); do kill -9 "$p" 2>/dev/null; done
    sleep 2
}

vm_is_alive() {
    pgrep -f qemu-system-x86_64 > /dev/null
}

ensure_vm() {
    # Relaunches the primary VM (single Type-2 accelerator) if it died.
    if vm_is_alive && wait_ssh 10022 3; then
        return 0
    fi
    log "ensure_vm: VM not reachable, relaunching"
    kill_qemu
    (
        cd "${REPO}/build" && \
        NUM_TYPE2=1 \
        SSH_PORT=10022 \
        DIRECTORY_ENTRIES=4096 \
        VM_LOG="${OVERNIGHT}/vm_primary.log" \
        nohup bash "${REPO}/qemu_integration/launch_qemu_type2_multigpu.sh" \
            > /dev/null 2>&1 &
    )
    if ! wait_ssh 10022 120; then
        log "ensure_vm: VM failed to boot within 6 minutes"
        log "ensure_vm: vm_primary.log tail:"
        tail -5 "${OVERNIGHT}/vm_primary.log" 2>/dev/null | tee -a "${LOG_MASTER}"
        return 1
    fi
    log "ensure_vm: VM back up"
}

aggregate_all() {
    log "aggregate: regenerating CSVs + report"
    "${PY}" "${AGG}" --logdir "${ART}/runs" --outdir "${ART}/csv" \
        >> "${LOG_MASTER}" 2>&1 || true
    "${PY}" "${REPORT}" --csv-dir "${ART}/csv" --outdir "${ART}/report" \
        >> "${LOG_MASTER}" 2>&1 || true
}

run_stage() {
    local name=$1; shift
    log "stage ${name} START"
    ensure_vm || { log "stage ${name} SKIP (VM boot failed)"; return 1; }
    # Preserve env assignments (e.g. RQ1_EXPS=1) the caller prefixed. Bash
    # passes those to the command automatically; we just need to re-invoke.
    if ! "${PY}" "$@" >> "${OVERNIGHT}/${name}.log" 2>&1; then
        log "stage ${name} FAILED (see ${OVERNIGHT}/${name}.log)"
        return 2
    fi
    log "stage ${name} DONE"
    aggregate_all
}

log "=== Splash overnight orchestration start ==="
log "logs under: ${OVERNIGHT}/"

# -- Stage 1a: RQ1 BFS only (fast; scales with N) ----------------------------
# Skip experiment2 (btree) and experiment3 (hash) during the N-scaling sweep
# because they have internal loops that don't depend on N and dominate wall
# time. We pick them up once at a single N below.
mkdir -p "${ART}/runs"
RQ1_EXPS=1 run_stage rq1_bfs_scaling \
    "${SWEEP}" \
    --only rq1 \
    --graph-sizes "100,200,500,1000" \
    --rq1-timeout 7200 \
    --logdir "${ART}/runs"

# -- Stage 1b: RQ1 btree + hash at a single N (slow, one shot) --------------
RQ1_EXPS=123 run_stage rq1_btree_hash \
    "${SWEEP}" --skip-push \
    --only rq1 \
    --graph-sizes "50" \
    --rq1-timeout 14400 \
    --logdir "${ART}/runs"

# -- Stage 2: RQ4 alloc-policy ----------------------------------------------
run_stage rq4_alloc \
    "${SWEEP}" --skip-push \
    --only rq4 \
    --alloc-sizes "500,2000" \
    --rq4-timeout 14400 \
    --logdir "${ART}/runs"

# -- Stage 3: RQ2 directory-capacity sweep ----------------------------------
kill_qemu
run_stage rq2_dir_capacity \
    "${RQ2}" \
    --capacities "64,256,1024,4096,16384" \
    --port 10022 \
    --timeout 3600

# -- Stage 4: Multi-GPU scaling sweep ---------------------------------------
kill_qemu
run_stage multigpu_scaling \
    "${MULTIGPU}" \
    --counts "1,2,4,8" \
    --only rq3 \
    --timeout 1800

log "=== Splash overnight orchestration complete ==="
aggregate_all
log "final report: ${ART}/report/splash_summary.md"
