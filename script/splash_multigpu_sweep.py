#!/usr/bin/env python3
"""
Splash multi-GPU scaling sweep.

Boots a VM with N Type-2 accelerator contexts attached (N in {1,2,4,6,8}),
runs one workload per N, captures timing + coherency stats. Produces
artifact/splash_sweep/multigpu_scaling/<N>/...

One VM + N devices per configuration (not N VMs). The workload runs
against the first device; other devices are present to stress the
directory and the bus, mimicking the scaling scenario described in the
Splash design notes.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shlex
import signal
import subprocess
import sys
import time

REPO = pathlib.Path("/home/victoryang00/CXLMemSim")
LAUNCHER = REPO / "qemu_integration" / "launch_qemu_type2_multigpu.sh"
SWEEP_DRIVER = REPO / "script" / "splash_sweep_driver.py"
ARTIFACT = REPO / "artifact" / "splash_sweep" / "multigpu_scaling"

SSH_BASE = [
    "-o", "ConnectTimeout=3",
    "-o", "BatchMode=yes",
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=/dev/null",
]


def wait_ssh(port: int, probes: int = 60, sleep: float = 3.0) -> bool:
    for i in range(probes):
        r = subprocess.run(
            ["ssh", *SSH_BASE, "-p", str(port), "root@127.0.0.1", "echo UP"],
            capture_output=True, text=True)
        if r.returncode == 0 and "UP" in r.stdout:
            return True
        time.sleep(sleep)
    return False


def kill_qemu():
    subprocess.run(["bash", "-c",
                    "for p in $(pgrep -f qemu-system-x86_64); do kill -9 $p; done"],
                   check=False)
    time.sleep(2)


def launch_vm(n: int, port: int, log_path: pathlib.Path,
              directory_entries: int) -> subprocess.Popen:
    env = os.environ.copy()
    env["NUM_TYPE2"] = str(n)
    env["SSH_PORT"] = str(port)
    env["DIRECTORY_ENTRIES"] = str(directory_entries)
    env["VM_LOG"] = str(log_path)
    return subprocess.Popen(
        ["bash", str(LAUNCHER)],
        cwd=str(REPO / "build"),
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )


def run_workload(n: int, port: int, outdir: pathlib.Path, only: str,
                 graph_sizes: str, timeout: int):
    outdir.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable, str(SWEEP_DRIVER),
        "--port", str(port),
        "--remote-dir", "/root/splash",
        "--logdir", str(outdir / "runs"),
        "--graph-sizes", graph_sizes,
        "--alloc-sizes", graph_sizes,
        "--only", only,
        "--rq1-timeout", str(timeout),
        "--rq3-timeout", str(timeout),
        "--rq4-timeout", str(timeout),
    ]
    subprocess.run(cmd, check=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--counts", default="1,2,4,6,8",
                    help="Comma list of accelerator counts")
    ap.add_argument("--port-base", type=int, default=10022)
    ap.add_argument("--only", default="rq1",
                    help="Which RQ workload to use at each count")
    ap.add_argument("--graph-sizes", default="500",
                    help="Graph sizes (applies to rq1/rq4_alloc)")
    ap.add_argument("--timeout", type=int, default=1800)
    ap.add_argument("--directory-entries", type=int, default=4096)
    ap.add_argument("--vm-boot-timeout-probes", type=int, default=60)
    args = ap.parse_args()

    counts = [int(x) for x in args.counts.split(",") if x.strip()]

    summary = []
    for idx, n in enumerate(counts):
        print(f"\n=== multi-GPU scaling: n={n} ===")
        port = args.port_base + idx
        outdir = ARTIFACT / f"n{n}"
        vm_log = outdir / "vm.log"
        outdir.mkdir(parents=True, exist_ok=True)
        vm_log.touch()

        kill_qemu()
        proc = launch_vm(n, port, vm_log, args.directory_entries)

        if not wait_ssh(port, probes=args.vm_boot_timeout_probes):
            print(f"  [skip n={n}] VM failed to boot (no SSH)")
            summary.append({"n": n, "status": "boot_failed"})
            kill_qemu()
            continue

        t0 = time.time()
        run_workload(n, port, outdir, args.only, args.graph_sizes, args.timeout)
        elapsed = time.time() - t0

        summary.append({"n": n, "port": port, "elapsed_sec": elapsed,
                        "status": "done", "outdir": str(outdir)})
        kill_qemu()

    manifest = ARTIFACT / "scaling_manifest.json"
    ARTIFACT.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(summary, indent=2))
    print(f"[manifest] {manifest}")


if __name__ == "__main__":
    main()
