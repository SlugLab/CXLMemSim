#!/usr/bin/env python3
"""Probe: PCI topology with 4 cxl-type2 devices under qtest."""
import socket
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "qemu_integration"))
from qtest_switch_offload import launch_qemu, scan_pci, stop_process  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
N = 4

with tempfile.TemporaryDirectory(prefix="t2probe-") as tmp:
    with socket.socket() as guard:
        guard.bind(("127.0.0.1", 0))
        port = guard.getsockname()[1]
    args = [str(ROOT / "lib/qemu/build-perf/qemu-system-x86_64"),
            "-qtest", f"unix:{tmp}/qtest.sock", "-qtest-log", "/dev/null",
            "-display", "none", "-audio", "none",
            "-machine", "q35,cxl=on", "-m", "512M", "-nodefaults",
            "-accel", "qtest",
            "-device", "pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.0"]
    for i in range(N):
        args += ["-device", f"cxl-rp,port={i},bus=cxl.0,id=rp{i},chassis=0,slot={i + 2},addr=0"]
        args += ["-device", f"cxl-type2,bus=rp{i},addr=0,id=t2{i},sn={200 + i},gpu-mode=0,"
                 f"cache-size=256K,mem-size=32M,latency-enabled=true,cxlmemsim-port={port}"]
    proc, qt, log = launch_qemu(args, Path(tmp) / "qtest.sock", Path(tmp) / "qemu.log")
    try:
        for d in sorted(scan_pci(qt, 40)):
            print(f"bus={d[0]:#04x} dev={d[1]:#04x} fn={d[2]} vendor={d[3]:#06x} "
                  f"device={d[4]:#06x} header={d[5]} class={d[6]:#08x}")
    finally:
        qt.close()
        stop_process(proc)
