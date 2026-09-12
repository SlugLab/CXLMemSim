#!/usr/bin/env python3
"""Smoke test: latency rails + notify-batch command on the patched device.

Verifies the wired timing model end-to-end before the full bench runs:
  - latency-enabled=on, rails 120/250/112 ns, 51 GB/s
  - repeated read of one line  -> hit accounting  (hit_ns/hits == 120)
  - read of fresh lines        -> miss accounting (232 ns = 120 + 112 fill)
  - write                      -> write accounting (250 ns)
  - NOTIFY_BATCH               -> modeled total = count*(250+1) + batches*112
  - bulk HTOD / DTOH           -> per-64B-line rails + serialized link time
  - DCOH install               -> one install per fill (240 = 232 + 8 ns)
  - allocate-on-miss=false     -> RdCurr class: no install, miss 232 ns
"""
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "qemu_integration"))
from qtest_switch_offload import (  # noqa: E402
    QTest, configure_pci_bridge, launch_qemu, pci_read, pci_write, stop_process,
)

ROOT = Path(__file__).resolve().parents[2]
BAR2 = 0x80000000
BAR4 = 0x90000000

DEV = ("cxl-type2,bus=rp,addr=0,id=t2,sn=201,gpu-mode=0,"
       "cache-size=256K,mem-size=64M,latency-enabled=true,"
       "read-latency-ns=120,write-latency-ns=250,coherency-latency-ns=112,"
       "bandwidth-gbps=51,hmc-install-ns=8,cxlmemsim-port={port}")

DEV_NOALLOC = ("cxl-type2,bus=rp,addr=0,id=t2,sn=201,gpu-mode=0,"
               "cache-size=256K,mem-size=64M,latency-enabled=true,"
               "read-latency-ns=120,write-latency-ns=250,"
               "coherency-latency-ns=112,bandwidth-gbps=51,hmc-install-ns=8,"
               "allocate-on-miss=false,cxlmemsim-port={port}")


def command(qt, cmd, *params):
    for i in range(8):
        qt.writeq(BAR2 + 0x40 + 8 * i, params[i] if i < len(params) else 0)
    qt.writel(BAR2 + 0x10, cmd)
    status = qt.readl(BAR2 + 0x14)
    result = qt.readl(BAR2 + 0x18)
    results = [qt.readq(BAR2 + 0x80 + 8 * i) for i in range(4)]
    data = [qt.readq(BAR2 + 0x1000 + 8 * i) for i in range(12)]
    assert status == 3 and result == 0, f"cmd {cmd:#x}: status={status} result={result}"
    return results, data


def launch_dev(tmp, dev_args, tag):
    import socket
    with socket.socket() as guard:
        guard.bind(("127.0.0.1", 0))
        port = guard.getsockname()[1]
    proc, qt, log = launch_qemu(
        [str(ROOT / "lib/qemu/build-perf/qemu-system-x86_64"),
         "-qtest", f"unix:{tmp}/qtest{tag}.sock", "-qtest-log", "/dev/null",
         "-display", "none", "-audio", "none",
         "-machine", "q35,cxl=on", "-m", "256M", "-nodefaults",
         "-accel", "qtest",
         "-device", "pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.0",
         "-device", "cxl-rp,port=0,bus=cxl.0,id=rp,chassis=0,slot=2,addr=0",
         "-device", dev_args.format(port=port)],
        Path(tmp) / f"qtest{tag}.sock", Path(tmp) / f"qemu{tag}.log")
    qt.sock.settimeout(30)
    configure_pci_bridge(qt, 12, 0, 0, 12, 13, 13, BAR2, 0x20000000)
    assert pci_read(qt, 13, 0, 0, 0) == 0x0D928086
    for reg, addr in [(0x18, BAR2), (0x20, BAR4)]:
        pci_write(qt, 13, 0, 0, reg, addr)
        pci_write(qt, 13, 0, 0, reg + 4, 0)
    pci_write(qt, 13, 0, 0, 4, pci_read(qt, 13, 0, 0, 4) | 6)
    assert qt.readl(BAR2) == 0x43584C32
    return proc, qt, log


def main():
    failures = []
    with tempfile.TemporaryDirectory(prefix="c2sight-smoke-") as tmp:
        # ---- phase 1: allocating device (defaults + install rail) ----
        proc, qt, log = launch_dev(tmp, DEV, "a")
        try:
            command(qt, 0xF2)  # timing reset

            # 3 reads of one line: first read misses (primes the cache,
            # 120+112=232 ns + 8 ns install), the next two hit (120 ns).
            for _ in range(3):
                qt.readq(BAR4 + 0x2000)
            r, d = command(qt, 0xF1)
            hits, hit_ns, misses, miss_ns = r[1], d[0], r[2], d[1]
            failures.append(("prime_miss", (misses, miss_ns), (1, 232)))
            failures.append(("hit_rail", (hits, hit_ns), (2, 240)))
            failures.append(("install_on_prime", (d[10], d[11]), (1, 8)))
            failures.append(("prime_acc", r[0], 232 + 8 + 2 * 120))

            # dedicated miss on a fresh line -> 232 ns + one 8 ns install
            command(qt, 0xF2)
            qt.readq(BAR4 + 0x2400)
            r, d = command(qt, 0xF1)
            failures.append(("miss_rail", (r[2], d[1]), (1, 232)))
            failures.append(("install_rail", (d[10], d[11]), (1, 8)))
            failures.append(("miss_plus_install_acc", r[0], 240))

            # write (line already cached): 250, no install
            qt.writeq(BAR4 + 0x2000, 7)
            r, d = command(qt, 0xF1)
            wc, wns = d[2], d[3]
            failures.append(("write_rail", (wc, wns), (1, 250)))
            failures.append(("no_install_on_hit_write", (d[10], d[11]), (1, 8)))

            # notify: 4096 notifications, batch 64, 64B payload
            # total = 4096 * (250 + 64//51) + 64 * 112 = 4096*251 + 7168
            command(qt, 0xF2)
            r, _ = command(qt, 0xF0, 0x300000, 4096, 64, 64)
            per, total, batches = r[2], r[1], r[3]
            failures.append(("notify_total", total, 4096 * 251 + 64 * 112))
            failures.append(("notify_per", per, (4096 * 251 + 64 * 112) // 4096))
            failures.append(("notify_batches", batches, 64))

            # bulk HTOD 640 B (10 lines): 10*250 + 640//51 = 2512
            command(qt, 0xF2)
            command(qt, 0x22, 0x400000, 640)
            _, d = command(qt, 0xF1)
            failures.append(("bulk_htod", d[9], 10 * 250 + 640 // 51))
            # bulk DTOH 640 B: 10*120 + 640//51 = 1212
            command(qt, 0x23, 0x400000, 640)
            _, d = command(qt, 0xF1)
            failures.append(("bulk_dtoh", d[8], 10 * 120 + 640 // 51))
        finally:
            qt.close()
            stop_process(proc)
            if log:
                log.close()

        # ---- phase 2: non-allocating device (RdCurr class) ----
        proc, qt, log = launch_dev(tmp, DEV_NOALLOC, "b")
        try:
            command(qt, 0xF2)
            qt.readq(BAR4 + 0x2000)
            r, d = command(qt, 0xF1)
            failures.append(("noalloc_miss", (r[2], d[1]), (1, 232)))
            failures.append(("noalloc_no_install", (d[10], d[11]), (0, 0)))
            failures.append(("noalloc_acc", r[0], 232))
            # re-read still misses (nothing cached)
            qt.readq(BAR4 + 0x2000)
            r, d = command(qt, 0xF1)
            failures.append(("noalloc_still_miss", (r[2], d[10]), (2, 0)))
        finally:
            qt.close()
            stop_process(proc)
            if log:
                log.close()

    ok = True
    for name, actual, expected in failures:
        match = actual == expected
        ok &= match
        print(f"{name}: actual={actual} expected={expected} {'PASS' if match else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
