#!/usr/bin/env bash
# Launch QEMU with the cxl-type2 device using the C2 Sight CXL Type-2
# calibrated properties (paper.pdf SPR + Agilex testbed profile).
#
# Differences from qemu_integration defaults:
#   cache-size=256K      HMC is 256KiB in the paper (default 128M)
#   x-speed=32 x-width=16 x-256b-flit=off   PCIe 5.0 x16, CXL 2.0 68B flits
#   hdm-db=false         the paper's device has no device-side directory;
#                        HDM-DB also requires 256B flit mode in this model
#   gfam-latency-ns=170 / gfam-bandwidth-mbps=52224 / mhsld-coh-latency-ns=112
#                        calibrated stage costs (consumed when gfam/mhsld on)
#
# Functional verification (no CXL timing -- qtest is transport-bound):
#   qemu_integration/qtest_type2_paper.py
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

QEMU="${QEMU_BIN:-$ROOT/lib/qemu/build-perf/qemu-system-x86_64}"
MEM_SIZE="${CXL_T2_MEM_SIZE:-32G}"      # HDM capacity (not reported in paper)
PORT="${CXL_MEMSIM_PORT:-9999}"
EXTRA_DISKS=()
[[ -f qemu1.img ]] && EXTRA_DISKS=(-drive "file=qemu1.img,index=0,media=disk,format=raw")

DEV="cxl-type2,bus=root_port13,id=cxl-type2-gpu0,sn=0x2"
DEV+=",cache-size=256K,mem-size=${MEM_SIZE}"
DEV+=",x-speed=32,x-width=16,x-256b-flit=off,hdm-db=false"
DEV+=",gfam-latency-ns=170,gfam-bandwidth-mbps=52224"
DEV+=",mhsld-coh-latency-ns=112"
DEV+=",coherency-enabled=true"
# Calibrated timing rails (wired as of the latency-model patch; see README):
DEV+=",latency-enabled=true"
DEV+=",read-latency-ns=120,write-latency-ns=250,coherency-latency-ns=112"
DEV+=",bandwidth-gbps=51"
DEV+=",cxlmemsim-addr=127.0.0.1,cxlmemsim-port=${PORT}"

exec "$QEMU" \
    -machine q35,cxl=on,cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size="$MEM_SIZE" \
    -m 16G,maxmem=32G,slots=8 -smp 4 \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1 \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=0 \
    -device "$DEV" \
    -nographic "${EXTRA_DISKS[@]}"
