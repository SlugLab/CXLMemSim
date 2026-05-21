#!/usr/bin/env python3
"""Run gem5-CXL with CXLMemSim-friendly defaults.

This wrapper intentionally keeps the gem5 configuration script selectable.
SlugLab/gem5-CXL is a gem5 fork, so local checkouts may carry different CXL
config names. The wrapper locates a gem5 binary, passes CXL/CXLMemSim runtime
environment variables, and enables O3CPU/LSQ/MemDepUnit debug traces that the
ROB and memory-stall tests consume.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys


DEFAULT_DEBUG_FLAGS = "O3CPU,LSQ,MemDepUnit"


def existing_path(value: str | None) -> Path | None:
    if not value:
        return None
    path = Path(value).expanduser()
    return path if path.exists() else None


def find_gem5_binary(gem5_root: Path | None, explicit: str | None) -> Path:
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit).expanduser())
    env_bin = os.environ.get("GEM5_BIN")
    if env_bin:
        candidates.append(Path(env_bin).expanduser())
    if gem5_root:
        candidates.extend(
            [
                gem5_root / "build" / "X86" / "gem5.opt",
                gem5_root / "build" / "ALL" / "gem5.opt",
                gem5_root / "build" / "X86" / "gem5.fast",
                gem5_root / "build" / "ALL" / "gem5.fast",
            ]
        )

    for candidate in candidates:
        if candidate.exists() and os.access(candidate, os.X_OK):
            return candidate

    searched = ", ".join(str(path) for path in candidates) or "no candidates"
    raise SystemExit(f"Unable to find an executable gem5 binary; searched: {searched}")


def find_config(gem5_root: Path | None, explicit: str | None) -> Path:
    if explicit:
        config = Path(explicit).expanduser()
        if config.exists():
            return config
        raise SystemExit(f"gem5 config script does not exist: {config}")

    if not gem5_root:
        raise SystemExit("Pass --config or --gem5-root so a gem5 config can be selected")

    candidates = [
        gem5_root / "configs" / "example" / "cxl.py",
        gem5_root / "configs" / "example" / "gem5_library" / "cxl_memory.py",
        gem5_root / "configs" / "example" / "gem5_library" / "x86-ubuntu-run-with-kvm.py",
        gem5_root / "configs" / "example" / "se.py",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate

    raise SystemExit(
        "Unable to auto-detect a gem5 config. Pass --config explicitly for this gem5-CXL checkout."
    )


def append_if_set(args: list[str], flag: str, value: str | None) -> None:
    if value:
        args.extend([flag, value])


def build_command(parsed: argparse.Namespace, extra_args: list[str]) -> tuple[list[str], dict[str, str]]:
    gem5_root = existing_path(parsed.gem5_root)
    gem5_bin = find_gem5_binary(gem5_root, parsed.gem5_bin)
    config = find_config(gem5_root, parsed.config)

    outdir = Path(parsed.outdir).expanduser()
    outdir.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(gem5_bin),
        f"--outdir={outdir}",
        f"--debug-flags={parsed.debug_flags}",
        f"--debug-file={parsed.debug_file}",
        str(config),
    ]

    append_if_set(cmd, "--kernel", parsed.kernel)
    append_if_set(cmd, "--disk-image", parsed.disk_image)
    append_if_set(cmd, "--script", parsed.script)
    append_if_set(cmd, "--cmd", parsed.cmd)
    append_if_set(cmd, "--mem-size", parsed.mem_size)
    append_if_set(cmd, "--cxl-mem-size", parsed.cxl_mem_size)

    cmd.extend(extra_args)

    env = os.environ.copy()
    env["CXL_MEMSIM_HOST"] = parsed.cxl_host
    env["CXL_MEMSIM_PORT"] = str(parsed.cxl_port)
    env["CXL_MEMSIM_TRANSPORT"] = parsed.cxl_transport
    env["CXL_DCD_ENABLE"] = "1" if parsed.enable_dcd else "0"
    env["CXL_GFAM_ENABLE"] = "1" if parsed.enable_gfam else "0"
    env["CXL_GFAM_HOST_ID"] = str(parsed.gfam_host_id)
    return cmd, env


def parse_args(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(
        description="Run SlugLab/gem5-CXL with CXLMemSim environment and O3CPU debug tracing."
    )
    parser.add_argument("--gem5-root", default=os.environ.get("GEM5_ROOT"))
    parser.add_argument("--gem5-bin", default=None)
    parser.add_argument("--config", default=None)
    parser.add_argument("--outdir", default="m5out-cxlmemsim")
    parser.add_argument("--kernel", default=None)
    parser.add_argument("--disk-image", default=None)
    parser.add_argument("--script", default=None)
    parser.add_argument("--cmd", default=None)
    parser.add_argument("--mem-size", default=None)
    parser.add_argument("--cxl-mem-size", default=None)
    parser.add_argument("--debug-flags", default=DEFAULT_DEBUG_FLAGS)
    parser.add_argument("--debug-file", default="o3cpu.trace")
    parser.add_argument("--cxl-host", default="127.0.0.1")
    parser.add_argument("--cxl-port", type=int, default=9999)
    parser.add_argument("--cxl-transport", choices=["tcp", "shm", "rdma"], default="tcp")
    parser.add_argument("--enable-dcd", action="store_true")
    parser.add_argument("--enable-gfam", action="store_true")
    parser.add_argument("--gfam-host-id", type=int, default=0)
    parser.add_argument("--dry-run", action="store_true")

    parsed, extra = parser.parse_known_args(argv)
    if extra and extra[0] == "--":
        extra = extra[1:]
    return parsed, extra


def main(argv: list[str]) -> int:
    parsed, extra = parse_args(argv)
    cmd, env = build_command(parsed, extra)

    print("gem5 command:")
    print(" ".join(shlex.quote(part) for part in cmd))
    print("CXLMemSim environment:")
    for key in [
        "CXL_MEMSIM_HOST",
        "CXL_MEMSIM_PORT",
        "CXL_MEMSIM_TRANSPORT",
        "CXL_DCD_ENABLE",
        "CXL_GFAM_ENABLE",
        "CXL_GFAM_HOST_ID",
    ]:
        print(f"  {key}={env[key]}")

    if parsed.dry_run:
        return 0

    return subprocess.run(cmd, env=env, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
