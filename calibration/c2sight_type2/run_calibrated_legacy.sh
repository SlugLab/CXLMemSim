#!/usr/bin/env bash
# Run application-level CXLMemSim with the C2 Sight CXL Type-2 calibrated
# parameters (see cxlmemsim_calibrated.json / README.md in this directory).
#
# Usage:
#   ./run_calibrated_legacy.sh [--target <binary>] [--extra "<CXLMemSim flags>"]
#
# The simulator must be built first:  cmake -B build && cmake --build build
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

TARGET="$ROOT/build/microbench/cache-miss"
EXTRA=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) TARGET="$2"; shift 2 ;;
        --extra)  EXTRA="$2";  shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

SIM="$ROOT/build/cxlmemsim_legacy"
[[ -x "$SIM" ]] || SIM="$(find "$ROOT/build" -maxdepth 3 \( -name cxlmemsim_legacy -o -name CXLMemSim \) -type f -executable | head -1)"
[[ -x "$SIM" ]] || { echo "cxlmemsim_legacy binary not found under $ROOT/build (make cxlmemsim_legacy)" >&2; exit 1; }

# C2 Sight SPR+Agilex Type-2 profile (paper.pdf):
#   -d 103    local DRAM latency (Section 4.1)
#   -f 4000   cores pinned at 4.0 GHz
#   -o "(1)"  single CXL endpoint = the Type-2 device's HDM
#             (two-endpoint variant: -o "(1,2)" -q 256,32,256 -l 220,250,165,190
#              -b 51,51,96,96 -- expander 2 = remote-socket DRAM, InterC rails)
#   -l 220,250  expander read/write latency (derived, see JSON)
#   -b 51,51    expander read/write BW = 51 GB/s CXL link ceiling
#   --mlc-bandwidth 51,51,46,0.80  link saturation model
# shellcheck disable=SC2086
exec "$SIM" \
    -d 103 -f 4000 \
    -o "(1)" \
    -q 256,32 \
    -l 220,250 \
    -b 51,51 \
    --mlc-bandwidth 51,51,46,0.80 \
    --bandwidth-window-ns 100000 \
    $EXTRA \
    -t "$TARGET"
