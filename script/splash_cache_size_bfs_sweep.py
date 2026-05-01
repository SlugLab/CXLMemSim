#!/usr/bin/env python3
"""Sweep CXL Type-2 cache size for RQ1 Graph BFS."""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import signal
import subprocess
import sys
import time

REPO = pathlib.Path("/home/victoryang00/CXLMemSim")
LAUNCHER = REPO / "qemu_integration" / "launch_qemu_type2_multigpu.sh"
SWEEP_DRIVER = REPO / "script" / "splash_sweep_driver.py"
ARTIFACT = REPO / "artifact" / "splash_sweep" / "cache_size_bfs"

SSH_BASE = [
    "-o", "ConnectTimeout=3",
    "-o", "BatchMode=yes",
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=/dev/null",
]


def wait_ssh(port: int, probes: int = 60, sleep_s: float = 3.0) -> bool:
    for _ in range(probes):
        proc = subprocess.run(
            ["ssh", *SSH_BASE, "-p", str(port), "root@127.0.0.1", "echo UP"],
            capture_output=True,
            text=True,
        )
        if proc.returncode == 0 and "UP" in proc.stdout:
            return True
        time.sleep(sleep_s)
    return False


def kill_qemu() -> None:
    proc = subprocess.run(["pgrep", "-f", "qemu-system-x86_64"], capture_output=True, text=True)
    for line in proc.stdout.splitlines():
        try:
            os.kill(int(line.strip()), signal.SIGKILL)
        except (ProcessLookupError, ValueError):
            pass
    time.sleep(2)


def launch_vm(cache_size: str, port: int, log_path: pathlib.Path, directory_entries: int) -> subprocess.Popen:
    env = os.environ.copy()
    env["NUM_TYPE2"] = "1"
    env["SSH_PORT"] = str(port)
    env["CXL_TYPE2_CACHE_SIZE"] = cache_size
    env["DIRECTORY_ENTRIES"] = str(directory_entries)
    env["VM_LOG"] = str(log_path)
    env["HETGPU_BACKEND"] = "5"
    return subprocess.Popen(
        ["bash", str(LAUNCHER)],
        cwd=str(REPO / "build"),
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )


def run_bfs(port: int, outdir: pathlib.Path, graph_size: int, timeout: int) -> None:
    outdir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["RQ1_EXPS"] = "1"
    cmd = [
        sys.executable, str(SWEEP_DRIVER),
        "--port", str(port),
        "--remote-dir", "/root/splash",
        "--logdir", str(outdir / "runs"),
        "--graph-sizes", str(graph_size),
        "--only", "rq1",
        "--rq1-timeout", str(timeout),
    ]
    subprocess.run(cmd, check=False, env=env)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache-sizes", default="16M,32M,64M,128M,256M,512M")
    parser.add_argument("--graph-size", type=int, default=2000)
    parser.add_argument("--port-base", type=int, default=10200)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--directory-entries", type=int, default=4096)
    args = parser.parse_args()

    cache_sizes = [s.strip() for s in args.cache_sizes.split(",") if s.strip()]
    summary = []

    for idx, cache_size in enumerate(cache_sizes):
        print(f"\n=== BFS cache-size sweep: cache={cache_size} ===")
        port = args.port_base + idx
        outdir = ARTIFACT / cache_size
        vm_log = outdir / "vm.log"
        outdir.mkdir(parents=True, exist_ok=True)

        kill_qemu()
        launch_vm(cache_size, port, vm_log, args.directory_entries)
        if not wait_ssh(port):
            print(f"  [skip cache={cache_size}] VM failed to boot")
            summary.append({"cache_size": cache_size, "status": "boot_failed"})
            kill_qemu()
            continue

        t0 = time.time()
        run_bfs(port, outdir, args.graph_size, args.timeout)
        summary.append({
            "cache_size": cache_size,
            "graph_size": args.graph_size,
            "port": port,
            "elapsed_sec": time.time() - t0,
            "status": "done",
            "outdir": str(outdir),
        })
        kill_qemu()

    ARTIFACT.mkdir(parents=True, exist_ok=True)
    manifest = ARTIFACT / "cache_size_manifest.json"
    manifest.write_text(json.dumps(summary, indent=2))
    print(f"[manifest] {manifest}")


if __name__ == "__main__":
    main()
