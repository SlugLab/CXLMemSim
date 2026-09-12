#!/usr/bin/env python3
"""DCOH allocation-backpressure bench: allocating vs non-allocating streams.

Streams a working set through the device cache (HMC, 256 KiB) with
allocate-on-miss on (RdShared class: every cold line pays the media-fill rail
plus the DCOH install rail) and off (RdCurr class: no fill, no install).
Reproduces the C2 Sight headline structure: the install serialization bounds
the allocating fill rate at 64 B / install-ns (~8 GB/s) against the 51 GB/s
link -- the 6.2x allocating-vs-non-allocating gap.

Output: notify_bench_results/dcoh_bench.json/.csv
"""
from __future__ import annotations

import argparse
import csv
import json
import socket
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "qemu_integration"))
from qtest_switch_offload import (  # noqa: E402
    configure_pci_bridge, launch_qemu, pci_read, pci_write, stop_process,
)

ROOT = Path(__file__).resolve().parents[2]
BAR2 = 0x80000000
BAR4 = 0x90000000

WORKING_SETS = [64 * 1024, 256 * 1024, 1024 * 1024, 4 * 1024 * 1024,
                16 * 1024 * 1024]
CACHE = 256 * 1024
ROUNDS = 2   # passes over the working set


def make_dev_args(port, allocate):
    return ("cxl-type2,bus=rp,addr=0,id=t2,sn=201,gpu-mode=0,"
            "cache-size=256K,mem-size=64M,latency-enabled=true,"
            "read-latency-ns=120,write-latency-ns=250,coherency-latency-ns=112,"
            "bandwidth-gbps=51,hmc-install-ns=8,"
            f"allocate-on-miss={'true' if allocate else 'false'},"
            f"cxlmemsim-port={port}")


def map_device(qt):
    configure_pci_bridge(qt, 12, 0, 0, 12, 13, 13, BAR2, 0x20000000)
    if pci_read(qt, 13, 0, 0, 0) != 0x0D928086:
        raise RuntimeError("Type-2 PCI identity mismatch")
    for reg, addr in [(0x18, BAR2), (0x20, BAR4)]:
        pci_write(qt, 13, 0, 0, reg, addr)
        pci_write(qt, 13, 0, 0, reg + 4, 0)
    pci_write(qt, 13, 0, 0, 4, pci_read(qt, 13, 0, 0, 4) | 6)
    assert qt.readl(BAR2) == 0x43584C32


def command(qt, cmd, *params):
    for i in range(8):
        qt.writeq(BAR2 + 0x40 + 8 * i, params[i] if i < len(params) else 0)
    qt.writel(BAR2 + 0x10, cmd)
    status = qt.readl(BAR2 + 0x14)
    result = qt.readl(BAR2 + 0x18)
    if status != 3 or result != 0:
        raise RuntimeError(f"cmd {cmd:#x}: status={status} result={result}")
    return [qt.readq(BAR2 + 0x80 + 8 * i) for i in range(4)]


def run_device(qemu, allocate, out, port, tmp):
    from qtest_switch_offload import launch_qemu as lq
    args = [str(qemu),
            "-qtest", f"unix:{tmp}/qtest.sock", "-qtest-log", "/dev/null",
            "-display", "none", "-audio", "none",
            "-machine", "q35,cxl=on", "-m", "512M", "-nodefaults",
            "-accel", "qtest",
            "-device", "pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.0",
            "-device", "cxl-rp,port=0,bus=cxl.0,id=rp,chassis=0,slot=2,addr=0",
            "-device", make_dev_args(port, allocate)]
    proc, qt, log = lq(args, Path(tmp) / "qtest.sock",
                       out / f"qemu_dcoh_{int(allocate)}.log")
    qt.sock.settimeout(120)
    rows = []
    try:
        map_device(qt)
        for ws in WORKING_SETS:
            lines = ws // 64
            command(qt, 0xF2)                      # timing reset
            before = command(qt, 0xF1)[0]          # latch acc_ns
            for _ in range(ROUNDS):
                for line in range(lines):
                    qt.readq(BAR4 + line * 64)
            after = command(qt, 0xF1)[0]
            r = command(qt, 0xF1)                  # relatch results
            misses = r[2]
            miss_ns = qt.readq(BAR2 + 0x1000 + 8 * 1)
            installs = qt.readq(BAR2 + 0x1000 + 8 * 10)
            per_line = (after - before) / (lines * ROUNDS)
            per_miss = miss_ns / misses if misses else 0.0
            rows.append({
                "allocate": allocate, "working_set": ws,
                "cache_bytes": CACHE, "lines": lines,
                "per_line_ns": per_line,
                "misses": misses, "per_miss_ns": per_miss,
                "installs": installs,
                "implied_fill_gbps": 64.0 / per_line if per_line else 0.0,
            })
            print(f"allocate={allocate} ws={ws >> 10:>5}KiB "
                  f"per_line={per_line:6.1f}ns per_miss={per_miss:5.1f}ns "
                  f"misses={misses} installs={installs} "
                  f"fill={64.0 / per_line:5.2f}GB/s", flush=True)
    finally:
        qt.close()
        stop_process(proc)
        if log:
            log.close()
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", type=Path,
                    default=ROOT / "lib/qemu/build-perf/qemu-system-x86_64")
    ap.add_argument("--out", type=Path,
                    default=Path(__file__).resolve().parent /
                    "notify_bench_results")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    rows = []
    with tempfile.TemporaryDirectory(prefix="c2sight-dcoh-") as tmp:
        with socket.socket() as guard:
            guard.bind(("127.0.0.1", 0))
            port = guard.getsockname()[1]
        for allocate in (True, False):
            rows += run_device(args.qemu, allocate, args.out, port, tmp)

    payload = {"schema": "c2sight.dcoh_bench.v1",
               "hmc_install_ns": 8, "bandwidth_gbps": 51,
               "rounds": ROUNDS, "rows": rows}
    (args.out / "dcoh_bench.json").write_text(json.dumps(payload, indent=2) + "\n")
    with (args.out / "dcoh_bench.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {args.out / 'dcoh_bench.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
