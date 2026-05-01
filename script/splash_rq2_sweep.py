#!/usr/bin/env python3
"""
RQ2: Directory-capacity sweep driver.

Reboots the VM at each directory-entries value so the Type-2 device is
re-instantiated with a different snoop-filter capacity, runs the rq2
binary, and collects per-config logs. Lives alongside
splash_sweep_driver.py; does not interfere with it.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys
import time

REPO = pathlib.Path("/home/victoryang00/CXLMemSim")
LAUNCHER = REPO / "qemu_integration" / "launch_qemu_type2_multigpu.sh"
SWEEP_DRIVER = REPO / "script" / "splash_sweep_driver.py"
ARTIFACT = REPO / "artifact" / "splash_sweep" / "rq2_dir_capacity"

SSH_BASE = ["-o","ConnectTimeout=3","-o","BatchMode=yes",
            "-o","StrictHostKeyChecking=no","-o","UserKnownHostsFile=/dev/null"]


def kill_qemu():
    subprocess.run(["bash","-c",
        "for p in $(pgrep -f qemu-system-x86_64); do kill -9 $p; done"],
        check=False)
    time.sleep(2)


def wait_ssh(port, probes=60, sleep=3.0):
    for _ in range(probes):
        r = subprocess.run(["ssh", *SSH_BASE, "-p", str(port),
                            "root@127.0.0.1", "echo UP"],
                           capture_output=True, text=True)
        if r.returncode == 0 and "UP" in r.stdout:
            return True
        time.sleep(sleep)
    return False


def launch_vm(port, vm_log, dir_entries):
    env = os.environ.copy()
    env.update({"NUM_TYPE2": "1", "SSH_PORT": str(port),
                "DIRECTORY_ENTRIES": str(dir_entries),
                "VM_LOG": str(vm_log)})
    return subprocess.Popen(["bash", str(LAUNCHER)],
        cwd=str(REPO / "build"), env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True)


def run_rq2(port, outdir, timeout):
    outdir.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, str(SWEEP_DRIVER),
           "--port", str(port),
           "--remote-dir", "/root/splash",
           "--logdir", str(outdir / "runs"),
           "--only", "rq2",
           "--rq2-timeout", str(timeout)]
    subprocess.run(cmd, check=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capacities", default="64,256,1024,4096,16384",
                    help="Comma list of directory entry counts")
    ap.add_argument("--port", type=int, default=10022)
    ap.add_argument("--timeout", type=int, default=1800)
    args = ap.parse_args()

    caps = [int(x) for x in args.capacities.split(",") if x.strip()]
    summary = []

    for cap in caps:
        print(f"\n=== RQ2: directory_entries={cap} ===")
        outdir = ARTIFACT / f"cap{cap}"
        vm_log = outdir / "vm.log"
        outdir.mkdir(parents=True, exist_ok=True)
        vm_log.touch()

        kill_qemu()
        launch_vm(args.port, vm_log, cap)
        if not wait_ssh(args.port):
            print(f"  [skip cap={cap}] VM boot failed")
            summary.append({"capacity": cap, "status": "boot_failed"})
            continue

        t0 = time.time()
        run_rq2(args.port, outdir, args.timeout)
        elapsed = time.time() - t0
        summary.append({"capacity": cap, "elapsed_sec": elapsed,
                        "status": "done", "outdir": str(outdir)})
        kill_qemu()

    manifest = ARTIFACT / "manifest.json"
    ARTIFACT.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps(summary, indent=2))
    print(f"[manifest] {manifest}")


if __name__ == "__main__":
    main()
