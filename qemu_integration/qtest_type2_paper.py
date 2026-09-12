#!/usr/bin/env python3
"""Reproducible Type-2 device checks and bias-cost measurements for IPDPS.

Runs the actual QEMU MMIO device, without a guest OS or physical CXL device.
Wall time includes the Python/qtest transport and is NOT CXL link latency.
Fresh QEMU processes isolate repeats. Failures remain in the output artifact.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path

from qtest_switch_offload import (
    QTest, configure_pci_bridge, launch_qemu, pci_read, pci_write,
    stop_process,
)

ROOT = Path(__file__).resolve().parents[1]
BAR2 = 0x80000000
BAR4 = 0x90000000
STAT_NAMES = [
    "snoop_hits", "snoop_misses", "coherency_requests", "back_invalidations",
    "writebacks", "evictions", "bias_flips", "device_bias_hits",
    "host_bias_hits", "upgrades", "downgrades", "directory_entries",
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as src:
        for block in iter(lambda: src.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def git_info(path: Path) -> dict:
    def read(*args):
        return subprocess.check_output(["git", "-C", str(path), *args], text=True)
    return {"commit": read("rev-parse", "HEAD").strip(),
            "status": read("status", "--short", "--untracked-files=no"),
            "diff_sha256": hashlib.sha256(read("diff", "HEAD").encode()).hexdigest()}


class Device:
    def __init__(self, qt: QTest):
        self.qt = qt

    def command(self, command: int, *params: int, allow_error=False):
        for index in range(8):
            self.qt.writeq(BAR2 + 0x40 + 8 * index, params[index] if index < len(params) else 0)
        self.qt.writel(BAR2 + 0x10, command)
        status = self.qt.readl(BAR2 + 0x14)
        result = self.qt.readl(BAR2 + 0x18)
        if not allow_error and (status != 3 or result != 0):
            raise RuntimeError(f"command {command:#x}: status={status}, result={result}")
        return result, [self.qt.readq(BAR2 + 0x80 + 8 * i) for i in range(4)]

    def stats(self):
        _, values = self.command(0xB0)
        values += [self.qt.readq(BAR2 + 0x1000 + i * 8) for i in range(8)]
        return dict(zip(STAT_NAMES, values))

    def reset(self):
        self.command(0xB1)

    def write(self, offset: int, value: int):
        self.qt.writeq(BAR4 + offset, value)

    def read(self, offset: int):
        return self.qt.readq(BAR4 + offset)

    def data_write(self, payload: bytes):
        self.qt.cmd(f"write {BAR2 + 0x1000:#x} {len(payload):#x} 0x{payload.hex()}")

    def data_read(self, size: int):
        value = self.qt.cmd(f"read {BAR2 + 0x1000:#x} {size:#x}")[1]
        return bytes.fromhex(value.removeprefix("0x"))


def map_device(qt):
    # The command line fixes root port at 0c:00.0 and endpoint at 0d:00.0.
    configure_pci_bridge(qt, 12, 0, 0, 12, 13, 13, BAR2, 0x20000000)
    if pci_read(qt, 13, 0, 0, 0) != 0x0D928086:
        raise RuntimeError("Type-2 PCI identity mismatch")
    for register, address in [(0x18, BAR2), (0x20, BAR4)]:
        pci_write(qt, 13, 0, 0, register, address)
        pci_write(qt, 13, 0, 0, register + 4, 0)
    pci_write(qt, 13, 0, 0, 4, pci_read(qt, 13, 0, 0, 4) | 6)
    if qt.readl(BAR2) != 0x43584C32:
        raise RuntimeError("BAR2 command interface not mapped")
    return Device(qt)


def checks(dev):
    rows = []

    def check(name, actual, expected):
        rows.append({"case": name, "actual": actual, "expected": expected,
                     "status": "pass" if actual == expected else "fail"})

    dev.reset()
    for offset in (0, 8, 56, 64, 0x200000, 0x200038):
        dev.write(offset, offset ^ 0x1122334455667788)
        check(f"bar4_readback_{offset:x}", dev.read(offset), offset ^ 0x1122334455667788)
    check("bar4_request_coverage", dev.stats()["coherency_requests"], 12)

    # CPU writes followed by simulated accelerator reads must agree.
    offset = 0x210000
    dev.write(offset, 17)
    dev.command(0x23, offset, 8)
    check("cpu_to_device_visibility", int.from_bytes(dev.data_read(8), "little"), 17)
    # Prime the CPU cache, then write from the device command path.
    dev.read(offset)
    dev.data_write(struct.pack("<Q", 29))
    dev.command(0x22, offset, 8)
    check("device_to_cpu_visibility", dev.read(offset), 29)
    # Explicit invalidate is a separate observation, never hidden recovery.
    dev.command(0x81, offset, 64)
    check("device_to_cpu_after_invalidate", dev.read(offset), 29)

    # Unaligned copies must invalidate both edge lines without losing bytes
    # outside the copied interval; this catches first-line-only repairs.
    edge = 0x214000
    for line in range(3):
        dev.write(edge + 64 * line, 0xAAAAAAAAAAAAAAAA)
    dev.data_write(bytes(range(80)))
    dev.command(0x22, edge + 60, 80)
    check("unaligned_copy_first_edge", dev.read(edge + 56), 0x0302010000000000)
    check("unaligned_copy_middle", dev.read(edge + 64), int.from_bytes(bytes(range(4, 12)), "little"))
    check("unaligned_copy_last_edge", dev.read(edge + 128), int.from_bytes(bytes(range(68, 76)), "little"))
    check("unaligned_copy_preserves_prefix", dev.read(edge), 0xAAAAAAAAAAAAAAAA)

    base = 0x220000
    dev.command(0xA4, base, 128, 0)
    dev.command(0xA4, base + 64, 64, 1)
    check("bias_host_line", dev.command(0xA5, base)[1][0], 0)
    check("bias_device_line", dev.command(0xA5, base + 64)[1][0], 1)
    dev.reset()
    dev.command(0xA6, base, 128, 1)
    check("flip_two_line_requests", dev.stats()["coherency_requests"], 2)
    check("flip_call_count", dev.stats()["bias_flips"], 1)
    check("bias_after_flip", dev.command(0xA5, base)[1][0], 1)
    dev.reset()
    check("counter_reset", dev.stats()["coherency_requests"], 0)

    _, info_before = dev.command(0xA2)
    _, allocation = dev.command(0xA0, 128)
    check("allocation_alignment", allocation[0] % 64, 0)
    dev.write(allocation[0], 91)
    check("allocated_readback", dev.read(allocation[0]), 91)
    dev.command(0xA1, allocation[0])
    _, info_after = dev.command(0xA2)
    check("free_restores_capacity", info_after[2], info_before[2])
    return rows


def bias_sweep(dev, repeat, quick):
    rows = []
    sizes = [64, 4096, 65536] if quick else [64, 256, 4096, 16384, 65536, 262144, 1048576]
    for size in sizes:
        for direction in (0, 1):
            dev.command(0xA4, 0x800000, size, 1 - direction)
            dev.reset()
            before = time.perf_counter_ns()
            dev.command(0xA6, 0x800000, size, direction)
            elapsed = time.perf_counter_ns() - before
            stats = dev.stats()
            expected = size // 64
            rows.append({"experiment": "bias_range", "repeat": repeat,
                         "size_bytes": size, "new_bias": direction,
                         "wall_ns": elapsed, **stats,
                         "expected_requests": expected,
                         "status": "pass" if stats["coherency_requests"] == expected and stats["bias_flips"] == 1 else "fail"})
    # Replay a four-region policy schedule. Data accesses are explicitly issued
    # via BAR4, so a zero-event static run is rejected by construction.
    steps = 128 if quick else 1024
    region_size = 4096
    bases = [0x1000000 + i * region_size for i in range(4)]
    for interval in (0, 16, 64, 256):
        for base in bases:
            dev.command(0xA4, base, region_size, 0)
        dev.reset()
        before = time.perf_counter_ns()
        flips = 0
        for step in range(steps):
            if interval and step and step % interval == 0:
                bias = (step // interval) % 2
                for base in bases:
                    dev.command(0xA6, base, region_size, bias)
                    flips += 1
            line = (step % (region_size // 64)) * 64
            for base in bases:
                dev.write(base + line, step)
                if dev.read(base + line) != step:
                    raise RuntimeError("policy replay data mismatch")
        elapsed = time.perf_counter_ns() - before
        stats = dev.stats()
        expected = steps * 8 + flips * (region_size // 64)
        rows.append({"experiment": "bias_schedule", "repeat": repeat,
                     "steps": steps, "interval_steps": interval,
                     "size_bytes": region_size * 4, "wall_ns": elapsed,
                     **stats, "expected_requests": expected,
                     "status": "pass" if stats["coherency_requests"] == expected and stats["bias_flips"] == flips else "fail"})
    # Same desired region assignment, with/without redundant-call suppression.
    # The replay is synchronous; this does not authorize eliding publication
    # fences in a concurrent workload.
    for policy in ("reassert", "guarded"):
        target = [1, 1, 0, 0]
        for base, bias in zip(bases, target):
            dev.command(0xA4, base, region_size, bias)
        current = target.copy()
        dev.reset()
        flips = 0
        before = time.perf_counter_ns()
        for step in range(steps):
            if step and step % 64 == 0:
                for index, base in enumerate(bases):
                    if policy == "reassert" or current[index] != target[index]:
                        dev.command(0xA6, base, region_size, target[index])
                        current[index] = target[index]
                        flips += 1
            for base in bases:
                offset = base + (step % 64) * 64
                dev.write(offset, step)
                if dev.read(offset) != step:
                    raise RuntimeError("guarded policy replay mismatch")
        elapsed = time.perf_counter_ns() - before
        stats = dev.stats()
        expected = steps * 8 + flips * 64
        rows.append({"experiment": "bias_reassert", "policy": policy,
                     "repeat": repeat, "steps": steps, "interval_steps": 64,
                     "size_bytes": region_size * 4, "wall_ns": elapsed,
                     **stats, "expected_requests": expected,
                     "status": "pass" if stats["coherency_requests"] == expected and stats["bias_flips"] == flips else "fail"})
    return rows


def directory_probe(dev, repeat):
    declared = dev.qt.readq(BAR2 + 0x318)
    before = dev.stats()["directory_entries"]
    dev.reset()
    start = time.perf_counter_ns()
    dev.command(0xA4, 0x2000000, (declared + 64) * 64, 1)
    elapsed = time.perf_counter_ns() - start
    stats = dev.stats()
    return {"experiment": "directory_capacity_audit", "repeat": repeat,
            "declared_entries": declared, "before_entries": before,
            "assigned_lines": declared + 64, "wall_ns": elapsed, **stats,
            "capacity_enforced": stats["directory_entries"] <= declared}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, default=ROOT / "lib/qemu/build-perf/qemu-system-x86_64")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    args.out.mkdir(parents=True, exist_ok=False)
    args.qemu = args.qemu.resolve()
    manifest = {"schema": "splash.ipdps27.qtest.v1", "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "platform": platform.platform(), "python": platform.python_version(),
                "repository": git_info(ROOT), "qemu_source": git_info(ROOT / "lib/qemu"),
                "qemu_binary": str(args.qemu), "qemu_sha256": sha256(args.qemu),
                "runner_sha256": sha256(Path(__file__)), "repeats": args.repeats,
                "quick": args.quick, "timing_scope": "Python/qtest end-to-end wall time; no CXL timing calibration",
                "gpu_mode": "simulated command source; no CUDA kernel", "server": "disconnected local functional mode"}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (args.out / "qemu-source.patch").write_bytes(subprocess.check_output(["git", "-C", str(ROOT / "lib/qemu"), "diff", "HEAD"]))
    results = []
    with tempfile.TemporaryDirectory(prefix="splash-ipdps-") as tmp:
        for repeat in range(args.repeats):
            proc = qt = log = None
            try:
                # Reserve an unused local TCP port: disconnected mode cannot
                # silently attach to somebody else's memory server.
                with socket.socket() as guard:
                    guard.bind(("127.0.0.1", 0))
                    port = guard.getsockname()[1]
                    command = [str(args.qemu), "-qtest", f"unix:{tmp}/qtest.sock", "-qtest-log", "/dev/null",
                               "-display", "none", "-audio", "none", "-machine", "q35,cxl=on", "-m", "256M",
                               "-nodefaults", "-accel", "qtest",
                               "-device", "pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.0",
                               "-device", "cxl-rp,port=0,bus=cxl.0,id=rp,chassis=0,slot=2,addr=0",
                               "-device", f"cxl-type2,bus=rp,addr=0,id=t2,sn=201,gpu-mode=0,cache-size=16M,mem-size=64M,cxlmemsim-port={port}"]
                    (args.out / f"command-{repeat}.json").write_text(json.dumps(command, indent=2) + "\n")
                    proc, qt, log = launch_qemu(command, Path(tmp) / "qtest.sock", args.out / f"qemu-{repeat}.log")
                    qt.sock.settimeout(30)
                    dev = map_device(qt)
                    functional = checks(dev)
                    rows = [directory_probe(dev, repeat)]
                    rows += bias_sweep(dev, repeat, args.quick)
                run = {"repeat": repeat, "functional": functional, "measurements": rows}
                results.append(run)
                (args.out / f"repeat-{repeat}.json").write_text(json.dumps(run, indent=2) + "\n")
                failures = [r["case"] for r in functional if r["status"] == "fail"]
                print(f"repeat={repeat} functional={len(functional) - len(failures)}/{len(functional)} failures={failures}", flush=True)
            finally:
                if qt:
                    qt.close()
                stop_process(proc)
                if log:
                    log.close()
    failed = any(row.get("status") == "fail" for run in results for row in run["functional"] + run["measurements"])
    summary = {"status": "fail" if failed else "pass", "results": results,
               "limitations": ["not a physical CXL measurement", "not a concurrent memory-model litmus test",
                               "directory capacity is audited, not assumed to be enforced"]}
    (args.out / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    sums = [f"{sha256(p)}  {p.relative_to(args.out)}" for p in sorted(args.out.iterdir()) if p.is_file()]
    (args.out / "SHA256SUMS").write_text("\n".join(sums) + "\n")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
