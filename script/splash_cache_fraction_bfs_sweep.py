#!/usr/bin/env python3
"""Sweep CXL Type-2 cache size and device fraction for large RQ1 Graph BFS."""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import time

REPO = pathlib.Path("/home/victoryang00/CXLMemSim")
LAUNCHER = REPO / "qemu_integration" / "launch_qemu_type2_multigpu.sh"
SWEEP_DRIVER = REPO / "script" / "splash_sweep_driver.py"
ARTIFACT = REPO / "artifact" / "splash_sweep" / "cache_fraction_bfs"

SSH_BASE = [
    "-o", "ConnectTimeout=3",
    "-o", "BatchMode=yes",
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=/dev/null",
]


def sanitize_unit_part(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "-", value).strip("-").lower()


def run(cmd: list[str], check: bool = False, timeout: int | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=str(REPO), text=True, capture_output=True, check=check, timeout=timeout)


def wait_ssh(port: int, probes: int = 80, sleep_s: float = 3.0) -> bool:
    for _ in range(probes):
        proc = run(["ssh", *SSH_BASE, "-p", str(port), "root@127.0.0.1", "echo UP"])
        if proc.returncode == 0 and "UP" in proc.stdout:
            return True
        time.sleep(sleep_s)
    return False


def systemctl_stop(unit: str) -> None:
    run(["sudo", "systemctl", "stop", f"{unit}.service"])
    time.sleep(2)


def launch_vm(cache_size: str, port: int, directory_entries: int, outdir: pathlib.Path, unit: str) -> bool:
    outdir.mkdir(parents=True, exist_ok=True)
    systemctl_stop(unit)
    cmd = [
        "sudo", "systemd-run",
        f"--unit={unit}",
        "--collect",
        f"--working-directory={REPO}",
        "env",
        "NUM_TYPE2=1",
        f"SSH_PORT={port}",
        "HETGPU_BACKEND=5",
        "CXL_MEMSIM_HOST=127.0.0.1",
        "CXL_MEMSIM_PORT=9999",
        f"CXL_TYPE2_CACHE_SIZE={cache_size}",
        f"DIRECTORY_ENTRIES={directory_entries}",
        f"VM_LOG={outdir / 'qemu_type2.log'}",
        f"MONITOR_SOCK=/tmp/{unit}.sock",
        str(LAUNCHER),
    ]
    proc = run(cmd)
    if proc.returncode != 0:
        print(f"  [error] systemd-run failed for cache={cache_size}: {proc.stderr.strip()}")
        return False
    return wait_ssh(port)


def run_bfs(port: int, outdir: pathlib.Path, graph_sizes: str, device_fractions: str,
            methods: str, timeout: int) -> dict:
    env = os.environ.copy()
    env["RQ1_EXPS"] = "1"
    env["RQ1_DEVICE_FRACTIONS"] = device_fractions
    env["RQ1_METHODS"] = methods
    cmd = [
        sys.executable, str(SWEEP_DRIVER),
        "--port", str(port),
        "--remote-dir", "/root/splash_cache_fraction_bfs",
        "--logdir", str(outdir / "runs"),
        "--graph-sizes", graph_sizes,
        "--only", "rq1",
        "--rq1-timeout", str(timeout),
    ]
    t0 = time.time()
    proc = subprocess.run(cmd, cwd=str(REPO), env=env, text=True, capture_output=True)
    (outdir / "driver.stdout.log").write_text(proc.stdout)
    (outdir / "driver.stderr.log").write_text(proc.stderr)
    return {
        "returncode": proc.returncode,
        "elapsed_sec": time.time() - t0,
        "driver_stdout": str(outdir / "driver.stdout.log"),
        "driver_stderr": str(outdir / "driver.stderr.log"),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache-sizes", default="512K,1M,2M,4M,8M,16M,32M")
    parser.add_argument("--graph-sizes", default="50000,100000")
    parser.add_argument("--device-fractions", default="0.0,0.5,1.0")
    parser.add_argument("--methods", default="A",
                        help="RQ1 methods to run: A pointer-sharing, B copy-based, or AB/all")
    parser.add_argument("--port-base", type=int, default=10400)
    parser.add_argument("--timeout", type=int, default=7200)
    parser.add_argument("--directory-entries", type=int, default=131072)
    args = parser.parse_args()

    cache_sizes = [s.strip() for s in args.cache_sizes.split(",") if s.strip()]
    summary = []
    ARTIFACT.mkdir(parents=True, exist_ok=True)

    for idx, cache_size in enumerate(cache_sizes):
        port = args.port_base + idx
        unit = f"cxl-bfs-cache-{sanitize_unit_part(cache_size)}"
        outdir = ARTIFACT / cache_size
        print(f"\n=== BFS cache/fraction sweep: cache={cache_size} port={port} ===", flush=True)

        booted = launch_vm(cache_size, port, args.directory_entries, outdir, unit)
        if not booted:
            summary.append({"cache_size": cache_size, "status": "boot_failed", "port": port, "unit": unit})
            systemctl_stop(unit)
            continue

        result = run_bfs(port, outdir, args.graph_sizes, args.device_fractions, args.methods, args.timeout)
        summary.append({
            "cache_size": cache_size,
            "graph_sizes": args.graph_sizes,
            "device_fractions": args.device_fractions,
            "methods": args.methods,
            "port": port,
            "unit": unit,
            "status": "done" if result["returncode"] == 0 else "driver_failed",
            **result,
        })
        print(f"  [done cache={cache_size}] rc={result['returncode']} elapsed={result['elapsed_sec']:.1f}s", flush=True)
        systemctl_stop(unit)

    manifest = ARTIFACT / "cache_fraction_manifest.json"
    manifest.write_text(json.dumps(summary, indent=2))
    print(f"[manifest] {manifest}")


if __name__ == "__main__":
    main()
