#!/bin/bash
# Follow-up orchestrator: run only the rq2 directory-capacity sweep and the
# multi-GPU scaling sweep. The main overnight script already captured rq1 +
# rq4_alloc; this exists so we don't reprocess those expensive stages.
set -u
REPO=/home/victoryang00/CXLMemSim
ART=${REPO}/artifact/splash_sweep
OVERNIGHT=${ART}/overnight
mkdir -p "${OVERNIGHT}"
LOG_MASTER=${OVERNIGHT}/followup.log

PY=${PY:-python3}
AGG=${REPO}/script/splash_aggregate.py
REPORT=${REPO}/script/splash_report.py
RQ2=${REPO}/script/splash_rq2_sweep.py
MULTIGPU=${REPO}/script/splash_multigpu_sweep.py

ts() { date "+%Y-%m-%d %H:%M:%S"; }
log() { echo "[$(ts)] $*" | tee -a "${LOG_MASTER}"; }

kill_qemu() {
    for p in $(pgrep -f "/home/victoryang00/CXLMemSim/lib/qemu/build/qemu-system-x86_64"); do
        kill -9 "$p" 2>/dev/null
    done
    sleep 2
}

aggregate_all() {
    log "aggregate: regenerating CSVs + report"
    "${PY}" "${AGG}" --logdir "${ART}/runs" --outdir "${ART}/csv" >> "${LOG_MASTER}" 2>&1 || true
    "${PY}" "${REPORT}" --csv-dir "${ART}/csv" --outdir "${ART}/report" >> "${LOG_MASTER}" 2>&1 || true
}

log "=== follow-up (rq2 + multi-GPU) start ==="

kill_qemu
log "stage rq2_dir_capacity START"
if "${PY}" "${RQ2}" \
    --capacities "64,256,1024,4096,16384" \
    --port 10022 \
    --timeout 3600 \
    >> "${OVERNIGHT}/rq2_dir_capacity.log" 2>&1; then
    log "stage rq2_dir_capacity DONE"
else
    log "stage rq2_dir_capacity FAILED"
fi
aggregate_all

kill_qemu
log "stage multigpu_scaling START"
if "${PY}" "${MULTIGPU}" \
    --counts "1,2,4,8" \
    --only rq3 \
    --timeout 1800 \
    >> "${OVERNIGHT}/multigpu_scaling.log" 2>&1; then
    log "stage multigpu_scaling DONE"
else
    log "stage multigpu_scaling FAILED"
fi

kill_qemu
aggregate_all
log "=== follow-up complete ==="
log "final report: ${ART}/report/splash_summary.md"
