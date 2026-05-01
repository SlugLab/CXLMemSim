#!/usr/bin/env python3
"""
Splash sweep driver.

Runs RQ1-RQ4 guest microbenchmarks inside a QEMU VM with the CXL Type-2 device
attached, captures stdout+stderr to per-run log files, writes a run manifest.

Assumes the VM is already booted and SSH is reachable. Defaults target the
user-mode-network hostfwd on 127.0.0.1:10022 (root account) created by this
session's launch command.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shlex
import subprocess
import sys
import time

REPO = pathlib.Path("/home/victoryang00/CXLMemSim")
GUEST_SRC = REPO / "qemu_integration" / "guest_libcuda"
ARTIFACT = REPO / "artifact" / "splash_sweep"

BINARIES = [
    "rq1_graph_bfs",
    "rq2_dir_sizing",
    "rq3_bias_kv",
    "rq4_alloc_policy",
    "rq4_devfrac_sweep",
]
LIBS = ["libcuda.so.1"]

SSH_BASE = [
    "-o", "ConnectTimeout=5",
    "-o", "StrictHostKeyChecking=no",
    "-o", "UserKnownHostsFile=/dev/null",
    "-o", "LogLevel=ERROR",
]


def ssh_cmd(host: str, port: int, user: str, remote: str) -> list[str]:
    return ["ssh", *SSH_BASE, "-p", str(port), f"{user}@{host}", remote]


def scp_cmd(host: str, port: int, user: str, src: str, dst: str) -> list[str]:
    return ["scp", *SSH_BASE, "-P", str(port), src, f"{user}@{host}:{dst}"]


def run(cmd: list[str], timeout: int | None = None) -> tuple[int, str, str]:
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout, p.stderr


def ensure_remote_dir(host, port, user, path):
    rc, _, err = run(ssh_cmd(host, port, user, f"mkdir -p {shlex.quote(path)}"))
    if rc != 0:
        raise SystemExit(f"ssh mkdir failed: {err}")


def push_binaries(host, port, user, remote_dir):
    ensure_remote_dir(host, port, user, remote_dir)
    for name in BINARIES + LIBS:
        src = str(GUEST_SRC / name)
        if not os.path.isfile(src):
            print(f"  [skip] {name} missing locally", file=sys.stderr)
            continue
        rc, _, err = run(scp_cmd(host, port, user, src, remote_dir + "/"))
        if rc != 0:
            print(f"  [warn] scp {name}: {err}", file=sys.stderr)
    # Symlink libcuda.so.1 to libcuda.so for -lcuda resolution if missing
    run(ssh_cmd(host, port, user,
                f"cd {remote_dir} && ln -sf libcuda.so.1 libcuda.so && "
                f"ln -sf libcuda.so.1 libnvcuda.so.1 && "
                f"ln -sf libnvcuda.so.1 libnvcuda.so && chmod +x " +
                " ".join(BINARIES)))


def run_experiment(host, port, user, remote_dir, logdir, run_key, binary, args, timeout):
    logdir.mkdir(parents=True, exist_ok=True)
    stdout_path = logdir / f"{run_key}.stdout.log"
    stderr_path = logdir / f"{run_key}.stderr.log"
    # Forward selected env vars to the guest so the workload binaries can be
    # steered from the host without rebuilding (RQ1_EXPS picks experiments).
    env_prefix_parts = ["LD_LIBRARY_PATH=."]
    for var in ("RQ1_EXPS", "RQ1_DEVICE_FRACTIONS", "RQ1_METHODS", "CXL_CUDA_DEBUG"):
        val = os.environ.get(var)
        if val:
            env_prefix_parts.append(f"{var}={shlex.quote(val)}")
    env_prefix = " ".join(env_prefix_parts)
    remote_cmd = (
        f"cd {shlex.quote(remote_dir)} && "
        f"{env_prefix} ./{binary} {args}"
    )
    cmd = ssh_cmd(host, port, user, remote_cmd)
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        elapsed = time.time() - t0
        stdout_path.write_text(p.stdout)
        stderr_path.write_text(p.stderr)
        return {
            "run_key": run_key,
            "binary": binary,
            "args": args,
            "returncode": p.returncode,
            "elapsed_sec": elapsed,
            "stdout_path": str(stdout_path),
            "stderr_path": str(stderr_path),
            "timed_out": False,
        }
    except subprocess.TimeoutExpired as e:
        elapsed = time.time() - t0
        def _s(x):
            if x is None: return ""
            return x.decode(errors="replace") if isinstance(x, (bytes, bytearray)) else x
        stdout_path.write_text(_s(e.stdout))
        stderr_path.write_text(_s(e.stderr) + "\n[TIMEOUT]\n")
        return {
            "run_key": run_key,
            "binary": binary,
            "args": args,
            "returncode": -1,
            "elapsed_sec": elapsed,
            "stdout_path": str(stdout_path),
            "stderr_path": str(stderr_path),
            "timed_out": True,
        }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=10022)
    ap.add_argument("--user", default="root")
    ap.add_argument("--remote-dir", default="/root/splash")
    ap.add_argument("--logdir", default=str(ARTIFACT / "runs"))
    ap.add_argument("--graph-sizes", default="2000,5000,10000,20000,50000",
                    help="Comma-separated N values for rq1")
    ap.add_argument("--alloc-sizes", default="2000,5000,10000",
                    help="Comma-separated N values for rq4_alloc_policy")
    ap.add_argument("--rq1-timeout", type=int, default=900)
    ap.add_argument("--rq2-timeout", type=int, default=1200)
    ap.add_argument("--rq3-timeout", type=int, default=600)
    ap.add_argument("--rq4-timeout", type=int, default=900)
    ap.add_argument("--skip-push", action="store_true")
    ap.add_argument("--only", default="", help="Comma list restrict to rq1,rq2,rq3,rq4,rq4df")
    args = ap.parse_args()

    logdir = pathlib.Path(args.logdir)
    only = {s.strip() for s in args.only.split(",") if s.strip()}

    if not args.skip_push:
        print(f"[push] binaries → {args.user}@{args.host}:{args.port}:{args.remote_dir}")
        push_binaries(args.host, args.port, args.user, args.remote_dir)

    manifest = []

    def run_and_record(**kw):
        rec = run_experiment(args.host, args.port, args.user, args.remote_dir,
                             logdir, **kw)
        manifest.append(rec)
        status = "TIMEOUT" if rec["timed_out"] else f"rc={rec['returncode']}"
        print(f"  [done] {rec['run_key']} {status} {rec['elapsed_sec']:.1f}s")

    graph_sizes = [int(s) for s in args.graph_sizes.split(",") if s]
    alloc_sizes = [int(s) for s in args.alloc_sizes.split(",") if s]

    if not only or "rq1" in only:
        for N in graph_sizes:
            run_and_record(run_key=f"rq1_N{N}", binary="rq1_graph_bfs",
                           args=str(N), timeout=args.rq1_timeout)

    if not only or "rq2" in only:
        run_and_record(run_key="rq2", binary="rq2_dir_sizing",
                       args="", timeout=args.rq2_timeout)

    if not only or "rq3" in only:
        run_and_record(run_key="rq3", binary="rq3_bias_kv",
                       args="", timeout=args.rq3_timeout)

    if not only or "rq4" in only:
        for N in alloc_sizes:
            run_and_record(run_key=f"rq4_alloc_N{N}", binary="rq4_alloc_policy",
                           args=str(N), timeout=args.rq4_timeout)

    if not only or "rq4df" in only:
        run_and_record(run_key="rq4_devfrac", binary="rq4_devfrac_sweep",
                       args="", timeout=args.rq4_timeout)

    manifest_path = logdir / "manifest.json"
    logdir.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2))
    print(f"[manifest] {manifest_path}")
    print(f"[done] {len(manifest)} runs")


if __name__ == "__main__":
    main()
