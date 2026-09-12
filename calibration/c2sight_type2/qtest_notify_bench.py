#!/usr/bin/env python3
"""CXL notification batch-size microbench against the patched cxl-type2 device.

Sweeps the number of notifications posted per batch (doorbell/completion
amortization) and records, per batch size:

  - device-modeled per-notification latency (ns)   [results[2] of NOTIFY_BATCH]
  - device-modeled effective throughput (GB/s)     [count*payload / modeled total]
  - qtest wall time for reference (transport-bound, NOT device time)

Device model (calibration/c2sight_type2, C2 Sight paper.pdf):
  per-notification = write-latency-ns (HDM media write, 250)
                     + payload / bandwidth-gbps (serialized link, 51 GB/s)
  per-batch        = coherency-latency-ns (doorbell/GO round trip, 112)

Also measures the calibrated rails (hit/miss/write) for the calibration graph.
Output: results JSON + CSV next to this script (or --out dir).
"""
from __future__ import annotations

import argparse
import csv
import json
import socket
import tempfile
import time
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "qemu_integration"))
from qtest_switch_offload import (  # noqa: E402
    configure_pci_bridge, launch_qemu, pci_read, pci_write, stop_process,
)

ROOT = Path(__file__).resolve().parents[2]
BAR2 = 0x80000000
BAR4 = 0x90000000

# Calibrated device: rails + bandwidth from cxlmemsim_calibrated.json.
DEV = ("cxl-type2,bus=rp,addr=0,id=t2,sn=201,gpu-mode=0,"
       "cache-size=256K,mem-size=256M,latency-enabled=true,"
       "read-latency-ns=120,write-latency-ns=250,coherency-latency-ns=112,"
       "bandwidth-gbps=51,cxlmemsim-port={port}")

BATCHES = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024]
COUNT = 262144          # notifications per experiment (256K * 64B = 16MB region)
PAYLOAD = 64            # bytes per notification (one cacheline event)
BASE = 0x400000         # event region offset in HDM (BAR4)


class Device:
    def __init__(self, qt):
        self.qt = qt

    def command(self, cmd, *params):
        for i in range(8):
            self.qt.writeq(BAR2 + 0x40 + 8 * i,
                           params[i] if i < len(params) else 0)
        self.qt.writel(BAR2 + 0x10, cmd)
        status = self.qt.readl(BAR2 + 0x14)
        result = self.qt.readl(BAR2 + 0x18)
        if status != 3 or result != 0:
            raise RuntimeError(f"cmd {cmd:#x}: status={status} result={result}")
        results = [self.qt.readq(BAR2 + 0x80 + 8 * i) for i in range(4)]
        data = [self.qt.readq(BAR2 + 0x1000 + 8 * i) for i in range(8)]
        return results, data

    def timing_reset(self):
        self.command(0xF2)

    def timing_get(self):
        return self.command(0xF1)

    def notify(self, count, batch, payload, base=BASE):
        t0 = time.perf_counter_ns()
        r, _ = self.command(0xF0, base, count, batch, payload)
        wall_ns = time.perf_counter_ns() - t0
        total, per, batches = r[1], r[2], r[3]
        return {
            "count": count, "batch": batch, "payload": payload,
            "modeled_total_ns": total,
            "modeled_per_notify_ns": per,
            "batches": batches,
            # Single outstanding stream: bytes per modeled ns == GB/s.  The
            # closed-loop stream is media-write-bound (250 ns rail); the
            # paper's 665 GB/s HDM media ceiling requires internal device
            # parallelism a synchronous single-stream bench does not model.
            "modeled_stream_gbps": count * payload / total if total else 0.0,
            "wall_ns": wall_ns,  # transport-bound reference only
        }


def map_device(qt):
    configure_pci_bridge(qt, 12, 0, 0, 12, 13, 13, BAR2, 0x40000000)
    if pci_read(qt, 13, 0, 0, 0) != 0x0D928086:
        raise RuntimeError("Type-2 PCI identity mismatch")
    for reg, addr in [(0x18, BAR2), (0x20, BAR4)]:
        pci_write(qt, 13, 0, 0, reg, addr)
        pci_write(qt, 13, 0, 0, reg + 4, 0)
    pci_write(qt, 13, 0, 0, 4, pci_read(qt, 13, 0, 0, 4) | 6)
    if qt.readl(BAR2) != 0x43584C32:
        raise RuntimeError("BAR2 command interface not mapped")
    return Device(qt)


def measure_rails(dev):
    """Device-cache hit / miss / write rails from the timing accounting."""
    rails = {}
    dev.timing_reset()
    for _ in range(3):
        dev.qt.readq(BAR4 + 0x2000)
    r, d = dev.timing_get()
    rails["hit_ns"] = d[0] // r[1] if r[1] else None      # 120 expected

    dev.timing_reset()
    dev.qt.readq(BAR4 + 0x2400)                            # fresh line
    r, d = dev.timing_get()
    rails["miss_ns"] = d[1] // r[2] if r[2] else None      # 232 = 120 + 112

    dev.timing_reset()
    dev.qt.writeq(BAR4 + 0x2000, 0x5A)
    r, d = dev.timing_get()
    rails["write_ns"] = d[3] // d[2] if d[2] else None     # 250
    return rails


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", type=Path,
                    default=ROOT / "lib/qemu/build-perf/qemu-system-x86_64")
    ap.add_argument("--out", type=Path,
                    default=Path(__file__).resolve().parent / "notify_bench_results")
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--count", type=int, default=COUNT)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="c2sight-notify-") as tmp:
        with socket.socket() as guard:
            guard.bind(("127.0.0.1", 0))
            port = guard.getsockname()[1]
        proc, qt, log = launch_qemu(
            [str(args.qemu),
             "-qtest", f"unix:{tmp}/qtest.sock", "-qtest-log", "/dev/null",
             "-display", "none", "-audio", "none",
             "-machine", "q35,cxl=on", "-m", "512M", "-nodefaults",
             "-accel", "qtest",
             "-device", "pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.0",
             "-device", "cxl-rp,port=0,bus=cxl.0,id=rp,chassis=0,slot=2,addr=0",
             "-device", DEV.format(port=port)],
            Path(tmp) / "qtest.sock", args.out / "qemu.log")
        qt.sock.settimeout(60)
        try:
            dev = map_device(qt)
            rails = measure_rails(dev)
            print(f"rails: {rails}", flush=True)

            runs = []
            for rep in range(args.repeats):
                for batch in BATCHES:
                    row = dev.notify(args.count, batch, PAYLOAD)
                    row["repeat"] = rep
                    runs.append(row)
                    print(f"rep={rep} batch={row['batch']:>5} "
                          f"per_notify={row['modeled_per_notify_ns']}ns "
                          f"stream={row['modeled_stream_gbps']:.3f}GB/s "
                          f"wall={row['wall_ns']/1e3:.0f}us", flush=True)
        finally:
            qt.close()
            stop_process(proc)
            if log:
                log.close()

    # Median across repeats per batch size.
    summary = {}
    for batch in BATCHES:
        rows = [r for r in runs if r["batch"] == batch]
        rows.sort(key=lambda r: r["modeled_per_notify_ns"])
        mid = rows[len(rows) // 2]
        summary[batch] = mid

    payload = {
        "schema": "c2sight.notify_bench.v1",
        "device_config": DEV,
        "rails": rails,
        "paper_rails": {"hit_ns": 120, "miss_ns": 232, "write_ns": 250,
                        "coherency_ns": 112, "bandwidth_gbps": 51},
        "count": args.count,
        "batches": BATCHES,
        "runs": runs,
        "summary": {str(k): v for k, v in summary.items()},
    }
    (args.out / "notify_bench.json").write_text(json.dumps(payload, indent=2) + "\n")
    with (args.out / "notify_bench.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(runs[0].keys()))
        w.writeheader()
        w.writerows(runs)
    print(f"wrote {args.out / 'notify_bench.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
