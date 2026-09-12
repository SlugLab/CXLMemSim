#!/usr/bin/env python3
"""Motivation experiment: coherent Type-2 sharing vs explicit publication.

Per-update cost of delivering a K-byte object update from a host producer to
the device consumer, three strategies, all charged by the device's calibrated
timing accounting (rails 120/250/112 ns, 51 GB/s):

  share      coherent sharing: producer's data writes stay in host memory;
             per update it posts one 64 B doorbell (BAR write, media-write
             rail) and the consumer pulls only the touched line through the
             device cache (read rail).  Cost is flat in K.
  publish    explicit publication: the producer pushes the full K bytes
             through the link (HTOD bulk: one media-write rail per 64 B line)
             before the doorbell and the consumer's read.
  replicate  full replication of a fixed 256 KiB shared structure per update.

Output: sharing_motivation.json + .csv
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

DEV = ("cxl-type2,bus=rp,addr=0,id=t2,sn=201,gpu-mode=0,"
       "cache-size=256K,mem-size=256M,latency-enabled=true,"
       "read-latency-ns=120,write-latency-ns=250,coherency-latency-ns=112,"
       "bandwidth-gbps=51,cxlmemsim-port={port}")

OBJECTS = [64, 256, 1024, 4096, 16384, 65536, 262144]
STRUCTURE = 262144       # full shared-structure size for the replicate strategy
UPDATES = 256            # updates per measurement
DOORBELL = 0x100000      # doorbell line offset in HDM
OBJ = 0x200000           # object region offset in HDM


class Device:
    def __init__(self, qt):
        self.qt = qt

    def command(self, cmd, *params):
        for i in range(8):
            self.qt.writeq(BAR2 + 0x40 + 8 * i,
                           params[i] if i < len(params) else 0)
        self.qt.writel(BAR2 + 0x10, cmd)
        status = self.qt.readl(BAR2 + 0x14)
        result = qt_ok = self.qt.readl(BAR2 + 0x18)
        if status != 3 or qt_ok != 0:
            raise RuntimeError(f"cmd {cmd:#x}: status={status} result={result}")
        return [self.qt.readq(BAR2 + 0x80 + 8 * i) for i in range(4)]

    def timing(self):
        self.command(0xF1)
        d = [self.qt.readq(BAR2 + 0x1000 + 8 * i) for i in range(10)]
        return {"acc_ns": self.qt.readq(BAR2 + 0x80),
                "hit_ns": d[0], "miss_ns": d[1], "write_ns": d[3],
                "notify_ns": d[4], "completion_ns": d[5],
                "bulk_read_ns": d[8], "bulk_write_ns": d[9]}

    def reset(self):
        self.command(0xF2)


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


def run_strategy(dev, strategy, obj_size, updates=UPDATES):
    """One measured phase; returns modeled ns per update from acc_ns deltas."""
    dev.reset()
    before = dev.timing()["acc_ns"]
    for u in range(updates):
        if strategy == "share":
            # Producer: doorbell only (data writes stayed in host memory).
            dev.qt.writeq(BAR4 + DOORBELL, u + 1)
            # Consumer: pull only the touched 64 B line through the cache.
            dev.command(0x23, OBJ + (u % 64) * 64, 64)
        elif strategy == "publish":
            # Producer: push the full object through the link, then doorbell.
            dev.command(0x22, OBJ, obj_size)
            dev.qt.writeq(BAR4 + DOORBELL, u + 1)
            # Consumer: read the touched line (device-local after the copy).
            dev.command(0x23, OBJ + (u % 64) * 64, 64)
        elif strategy == "replicate":
            dev.command(0x22, OBJ, STRUCTURE)
            dev.qt.writeq(BAR4 + DOORBELL, u + 1)
            dev.command(0x23, OBJ + (u % 64) * 64, 64)
        else:
            raise ValueError(strategy)
    total = dev.timing()["acc_ns"] - before
    return total / updates


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", type=Path,
                    default=ROOT / "lib/qemu/build-perf/qemu-system-x86_64")
    ap.add_argument("--out", type=Path,
                    default=Path(__file__).resolve().parent /
                    "notify_bench_results")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="c2sight-share-") as tmp:
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
            Path(tmp) / "qtest.sock", args.out / "qemu_sharing.log")
        qt.sock.settimeout(60)
        try:
            dev = map_device(qt)
            rows = []
            for k in OBJECTS:
                row = {"object_bytes": k}
                for strategy in ("share", "publish", "replicate"):
                    row[f"{strategy}_ns_per_update"] = run_strategy(
                        dev, strategy, k)
                row["publish_over_share"] = (
                    row["publish_ns_per_update"] / row["share_ns_per_update"])
                row["replicate_over_share"] = (
                    row["replicate_ns_per_update"] / row["share_ns_per_update"])
                rows.append(row)
                print(f"K={k:>7}  share={row['share_ns_per_update']:>8.1f}ns  "
                      f"publish={row['publish_ns_per_update']:>10.1f}ns "
                      f"({row['publish_over_share']:>6.1f}x)  "
                      f"replicate={row['replicate_ns_per_update']:>10.1f}ns "
                      f"({row['replicate_over_share']:>7.1f}x)", flush=True)
        finally:
            qt.close()
            stop_process(proc)
            if log:
                log.close()

    payload = {
        "schema": "c2sight.sharing_motivation.v1",
        "device_config": DEV,
        "updates_per_measurement": UPDATES,
        "structure_bytes": STRUCTURE,
        "rows": rows,
    }
    (args.out / "sharing_motivation.json").write_text(
        json.dumps(payload, indent=2) + "\n")
    with (args.out / "sharing_motivation.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {args.out / 'sharing_motivation.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
