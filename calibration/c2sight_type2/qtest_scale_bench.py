#!/usr/bin/env python3
"""Device-count scaling bench: 1..32 cxl-type2 endpoints under one qtest.

Maps N independent Type-2 endpoints (each behind its own CXL root port), then
measures, per device count:

  notify    per-device modeled notification stream (batch 64, 64 B events)
            and the aggregate modeled throughput across devices;
  share     per-update modeled cost of the coherent-sharing strategy
            (doorbell + on-demand device-cache read), aggregate update rate.

All timing is device-side modeled service time (latency-enabled rails), so
"aggregate" composes the independent per-device accounting; the serial qtest
driver wall time is recorded for reference only.  Cross-device contention
(link, switch, host) is not modeled in these local tests -- see the paper's
evidence boundaries.
"""
from __future__ import annotations

import argparse
import csv
import json
import socket
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "qemu_integration"))
from qtest_switch_offload import (  # noqa: E402
    configure_pci_bridge, launch_qemu, pci_read, pci_write, scan_pci,
    stop_process,
)

ROOT = Path(__file__).resolve().parents[2]
PXB_BUS = 0x0C
COUNTS = [1, 2, 4, 8, 16, 32]
NOTIFY_COUNT = 65536
NOTIFY_BATCH = 64
UPDATES = 256
OBJECT = 4096
WINDOW = 0x4000000          # 64 MiB per device (BAR2 2 MiB + BAR4 32 MiB)
BASE0 = 0x40000000
TYPE2_VENDOR = 0x8086
TYPE2_DEVICE = 0x0D92

DEV = ("cxl-type2,bus=rp{rp},addr=0,id=t2{i},sn={sn},gpu-mode=0,"
       "cache-size=256K,mem-size=32M,latency-enabled=true,"
       "read-latency-ns=120,write-latency-ns=250,coherency-latency-ns=112,"
       "bandwidth-gbps=51,cxlmemsim-port={port}")


class Device:
    def __init__(self, qt, bar2, bar4):
        self.qt = qt
        self.bar2 = bar2
        self.bar4 = bar4

    def command(self, cmd, *params):
        b2 = self.bar2
        for i in range(8):
            self.qt.writeq(b2 + 0x40 + 8 * i,
                           params[i] if i < len(params) else 0)
        self.qt.writel(b2 + 0x10, cmd)
        status = self.qt.readl(b2 + 0x14)
        result = self.qt.readl(b2 + 0x18)
        if status != 3 or result != 0:
            raise RuntimeError(f"cmd {cmd:#x}: status={status} result={result}")
        return [self.qt.readq(b2 + 0x80 + 8 * i) for i in range(4)]

    def notify(self, count, batch, payload, base=0x400000):
        r = self.command(0xF0, base, count, batch, payload)
        return r[1]  # modeled total ns

    def share_updates(self, updates=UPDATES, obj=OBJECT, obj_base=0x200000,
                       doorbell=0x100000):
        self.command(0xF2)
        before = self.command(0xF1)[0]
        for u in range(updates):
            self.qt.writeq(self.bar4 + doorbell, u + 1)
            self.command(0x23, obj_base + (u % 64) * 64, 64)
        return (self.command(0xF1)[0] - before) / updates

    def hit_rail(self):
        """Prime a line, then measure one hit via acc_ns delta (sanity)."""
        self.qt.readq(self.bar4 + 0x2000)
        self.command(0xF2)
        self.qt.readq(self.bar4 + 0x2000)
        return self.command(0xF1)[0]


def launch(n, port, tmp, qemu, out):
    """N endpoints: one pxb-cxl per 16 root ports (per-bus addr limit)."""
    args = [str(qemu),
            "-qtest", f"unix:{tmp}/qtest.sock", "-qtest-log", "/dev/null",
            "-display", "none", "-audio", "none",
            "-machine", "q35,cxl=on", "-m", "512M", "-nodefaults",
            "-accel", "qtest"]
    per_bus = 16
    nbuses = (n + per_bus - 1) // per_bus
    pxb_buses = []
    next_bus = 12
    for b in range(nbuses):
        args += ["-device", f"pxb-cxl,bus_nr={next_bus},bus=pcie.0,id=cxl.{b}"]
        pxb_buses.append(next_bus)
        count = min(per_bus, n - b * per_bus)
        for i in range(count):
            args += ["-device", f"cxl-rp,port={i},bus=cxl.{b},id=rp{b}_{i},"
                     f"chassis={b},slot={i + 2},addr={i + 2}"]
            args += ["-device", DEV.format(rp=f"{b}_{i}", i=b * per_bus + i,
                                           sn=200 + i, port=port)]
        # secondary buses are auto-assigned after all rps of this pxb
        next_bus = next_bus + 1 + count
    log_path = out / f"qemu_scale_{n}.log"
    return launch_qemu(args, Path(tmp) / "qtest.sock", log_path)


def map_devices(qt, n):
    """Configure each root port's bus window and each endpoint's BARs."""
    per_bus = 16
    rps = sorted((d for d in scan_pci(qt, 60)
                  if d[6] in (0x060400, 0x060403) and d[0] >= PXB_BUS
                  and d[1] >= 2),
                 key=lambda d: (d[0], d[1]))
    assert len(rps) == n, f"expected {n} root ports, got {len(rps)}"
    devices = []
    for j, (pbus, dev, fn, *_r) in enumerate(rps):
        sec = 13 + j + j // per_bus   # realization-order bus numbering
        base = BASE0 + j * WINDOW
        configure_pci_bridge(qt, pbus, dev, fn, pbus, sec, sec, base, WINDOW)
        matches = [d for d in scan_pci(qt, 60) if d[0] == sec
                   and d[3] == TYPE2_VENDOR and d[4] == TYPE2_DEVICE]
        if not matches:
            raise RuntimeError(f"no Type-2 endpoint on bus {sec:#x}")
        ebus, edev, efn, *_ = matches[0]
        bar2 = base
        bar4 = base + 0x2000000
        pci_write(qt, ebus, edev, efn, 0x18, bar2 & 0xFFFFFFFF)
        pci_write(qt, ebus, edev, efn, 0x1C, 0)
        pci_write(qt, ebus, edev, efn, 0x20, bar4 & 0xFFFFFFFF)
        pci_write(qt, ebus, edev, efn, 0x24, 0)
        command = pci_read(qt, ebus, edev, efn, 4)
        pci_write(qt, ebus, edev, efn, 4, command | 6)
        if qt.readl(bar2) != 0x43584C32:
            raise RuntimeError(f"device {j}: BAR2 magic mismatch")
        devices.append(Device(qt, bar2, bar4))
    return devices


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--qemu", type=Path,
                    default=ROOT / "lib/qemu/build-perf/qemu-system-x86_64")
    ap.add_argument("--out", type=Path,
                    default=Path(__file__).resolve().parent /
                    "notify_bench_results")
    ap.add_argument("--counts", type=int, nargs="+", default=COUNTS)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    rows = []
    for n in args.counts:
        with tempfile.TemporaryDirectory(prefix=f"t2scale{n}-") as tmp:
            with socket.socket() as guard:
                guard.bind(("127.0.0.1", 0))
                port = guard.getsockname()[1]
            proc, qt, log = launch(n, port, tmp, args.qemu, args.out)
            qt.sock.settimeout(120)
            try:
                devices = map_devices(qt, n)
                rails = [d.hit_rail() for d in devices]
                assert all(r == 120 for r in rails), f"rail check: {rails}"

                t0 = time.perf_counter_ns()
                per_dev = [d.notify(NOTIFY_COUNT, NOTIFY_BATCH, 64)
                           for d in devices]
                wall_ns = time.perf_counter_ns() - t0
                bytes_total = n * NOTIFY_COUNT * 64
                slowest = max(per_dev)
                agg_gbps = bytes_total / slowest  # modeled parallel
                row = {"devices": n, "experiment": "notify",
                       "per_device_ns": slowest,
                       "agg_modeled_gbps": agg_gbps,
                       "scaling_efficiency": agg_gbps / n / 0.253,
                       "driver_wall_ns": wall_ns}
                rows.append(row)
                print(f"N={n:>2} notify: per_dev={slowest/1e6:.2f}ms "
                      f"agg={agg_gbps:.3f}GB/s "
                      f"eff={row['scaling_efficiency']*100:.0f}% "
                      f"wall={wall_ns/1e6:.0f}ms", flush=True)

                t0 = time.perf_counter_ns()
                per_upd = [d.share_updates() for d in devices]
                wall_ns = time.perf_counter_ns() - t0
                slowest = max(per_upd)
                row = {"devices": n, "experiment": "share",
                       "per_device_ns": slowest,
                       "agg_modeled_gbps": n * 64.0 * 64 / slowest,
                       "scaling_efficiency": slowest and per_upd[0] / slowest,
                       "driver_wall_ns": wall_ns}
                rows.append(row)
                print(f"N={n:>2} share:  per_update={slowest:.1f}ns "
                      f"eff={row['scaling_efficiency']*100:.0f}% "
                      f"wall={wall_ns/1e6:.0f}ms", flush=True)
            finally:
                qt.close()
                stop_process(proc)
                if log:
                    log.close()

    payload = {"schema": "c2sight.scale_bench.v1", "rows": rows,
               "notify_batch": NOTIFY_BATCH, "notify_count": NOTIFY_COUNT,
               "updates": UPDATES, "object_bytes": OBJECT}
    (args.out / "scale_bench.json").write_text(json.dumps(payload, indent=2) + "\n")
    with (args.out / "scale_bench.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {args.out / 'scale_bench.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
