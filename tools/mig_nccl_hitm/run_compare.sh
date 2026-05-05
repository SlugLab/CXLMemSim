#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NCCL_HOME="${NCCL_HOME:-/tmp/nccl-mig/build}"
CUDA_HOME="${CUDA_HOME:-/usr/local/cuda}"
LINES="${LINES:-64}"
ITERS="${ITERS:-1000}"
WARMUP="${WARMUP:-100}"
NDEVS="${NDEVS:-0}"

export LD_LIBRARY_PATH="${NCCL_HOME}/lib:${ROOT_DIR}/qemu_integration/guest_libcuda:${LD_LIBRARY_PATH:-}"
export NCCL_BYPASS_MIG_DUPLICATE_CHECK="${NCCL_BYPASS_MIG_DUPLICATE_CHECK:-1}"
export NCCL_P2P_DISABLE="${NCCL_P2P_DISABLE:-1}"

make -C "${ROOT_DIR}/tools/mig_nccl_hitm" CUDA_HOME="${CUDA_HOME}" NCCL_HOME="${NCCL_HOME}"
make -C "${ROOT_DIR}/qemu_integration/guest_libcuda" cpu_gpu_hitm_benchmark

echo "== MIG/NCCL GPU-side HITM =="
"${ROOT_DIR}/tools/mig_nccl_hitm/mig_nccl_hitm_benchmark" "${LINES}" "${ITERS}" "${WARMUP}" "${NDEVS}" | tee /tmp/mig_nccl_hitm.out

echo
echo "== CXL simulator CPU/GPU HITM =="
"${ROOT_DIR}/qemu_integration/guest_libcuda/cpu_gpu_hitm_benchmark" "${LINES}" "${ITERS}" "${WARMUP}" | tee /tmp/cxl_sim_hitm.out

echo
echo "== Extracted comparison =="
awk '
  /^\[/ { mode=$0 }
  /avg_line_handoff_ns=/ { print FILENAME, mode, $0 }
' /tmp/mig_nccl_hitm.out /tmp/cxl_sim_hitm.out
