#!/usr/bin/env python3
"""Run Kimi-style ternary/NCCL-hook size scaling on the emulated CXL switch."""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class KimiProfile:
    name: str
    layers: int
    ranks: int
    dim: int
    kv_shard_bytes: int
    reduce_count: int


FIELD_ORDER = [
    "profile",
    "layers",
    "ranks",
    "dim",
    "kv_shard_bytes",
    "reduce_count",
    "case",
    "commands",
    "bytes",
    "work_items",
    "modeled_latency_ns",
    "queued_ns",
    "service_ns",
    "general_ops",
    "ai_ops",
    "general_bytes",
    "ai_bytes",
    "result",
    "status",
    "damer_trace",
]


def profiles(preset: str) -> list[KimiProfile]:
    base = [
        KimiProfile("tiny", 1, 2, 4, 128, 16),
        KimiProfile("small", 2, 4, 4, 256, 32),
        KimiProfile("balanced", 3, 4, 8, 256, 64),
        KimiProfile("deep", 6, 4, 8, 512, 64),
        KimiProfile("rank_scale", 3, 8, 8, 256, 64),
        KimiProfile("dim_scale", 3, 4, 16, 512, 128),
    ]
    if preset == "standard":
        return base[:3]
    if preset == "all":
        return base
    raise RuntimeError(f"unknown preset {preset!r}")


def run_one(args: argparse.Namespace, profile: KimiProfile, index: int) -> dict[str, str]:
    run_dir = Path(args.run_dir) / profile.name
    run_dir.mkdir(parents=True, exist_ok=True)
    output_prefix = "kimi_size"
    cmd = [
        sys.executable,
        args.benchmark_script,
        f"--port={args.base_port + index}",
        f"--run-dir={run_dir}",
        f"--server={args.server}",
        f"--qemu={args.qemu}",
        f"--damer-root={args.damer_root}",
        "--cases=kimi26_ternary_nccl_hook_e2e",
        f"--output-prefix={output_prefix}",
        f"--kimi-layers={profile.layers}",
        f"--kimi-ranks={profile.ranks}",
        f"--kimi-dim={profile.dim}",
        f"--kimi-kv-shard-bytes={profile.kv_shard_bytes}",
        f"--kimi-reduce-count={profile.reduce_count}",
        f"--switch-general-cores={args.switch_general_cores}",
        f"--switch-ai-cores={args.switch_ai_cores}",
        f"--switch-general-latency={args.switch_general_latency}",
        f"--switch-ai-latency={args.switch_ai_latency}",
        f"--switch-general-bandwidth={args.switch_general_bandwidth}",
        f"--switch-ai-ops-per-ns={args.switch_ai_ops_per_ns}",
    ]
    proc = subprocess.run(cmd, cwd=Path(args.benchmark_script).parent,
                          text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    log_path = run_dir / "driver.log"
    with open(log_path, "w", encoding="utf-8") as fh:
        fh.write("$ " + " ".join(cmd) + "\n")
        fh.write(proc.stdout)
        if proc.stderr:
            fh.write("\nSTDERR\n")
            fh.write(proc.stderr)
    if proc.returncode != 0:
        print(proc.stdout, end="")
        print(proc.stderr, end="", file=sys.stderr)
        raise RuntimeError(f"profile {profile.name} failed; see {log_path}")

    csv_path = run_dir / f"{output_prefix}.csv"
    with open(csv_path, "r", encoding="utf-8", newline="") as fh:
        rows = list(csv.DictReader(fh))
    if len(rows) != 1:
        raise RuntimeError(f"profile {profile.name} returned {len(rows)} rows")
    row = rows[0]
    row.update({
        "profile": profile.name,
        "layers": str(profile.layers),
        "ranks": str(profile.ranks),
        "dim": str(profile.dim),
        "kv_shard_bytes": str(profile.kv_shard_bytes),
        "reduce_count": str(profile.reduce_count),
    })
    return row


def write_outputs(rows: list[dict[str, str]], run_dir: Path,
                  selected_profiles: list[KimiProfile],
                  args: argparse.Namespace) -> tuple[Path, Path]:
    csv_path = run_dir / "kimi_size_sweep.csv"
    json_path = run_dir / "kimi_size_sweep.json"
    with open(csv_path, "w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=FIELD_ORDER)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in FIELD_ORDER})

    payload = {
        "config": {
            "preset": args.preset,
            "server": args.server,
            "qemu": args.qemu,
            "damer_root": args.damer_root,
            "switch_general_cores": args.switch_general_cores,
            "switch_ai_cores": args.switch_ai_cores,
            "switch_general_latency": args.switch_general_latency,
            "switch_ai_latency": args.switch_ai_latency,
            "switch_general_bandwidth": args.switch_general_bandwidth,
            "switch_ai_ops_per_ns": args.switch_ai_ops_per_ns,
        },
        "profiles": [profile.__dict__ for profile in selected_profiles],
        "results": rows,
    }
    with open(json_path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2)
    return csv_path, json_path


def print_summary(rows: list[dict[str, str]]) -> None:
    print("\nKIMI SIZE SUMMARY")
    print("profile,layers,ranks,dim,commands,latency_ns,queued_ns,general_ops,ai_ops,bytes,work_items")
    for row in rows:
        print(
            f"{row['profile']},{row['layers']},{row['ranks']},{row['dim']},"
            f"{row['commands']},{row['modeled_latency_ns']},{row['queued_ns']},"
            f"{row['general_ops']},{row['ai_ops']},{row['bytes']},{row['work_items']}"
        )


def main() -> int:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--preset", choices=["standard", "all"], default="all")
    parser.add_argument("--run-dir", default=str(repo / "build" / "qtest-switch-kimi26-size"))
    parser.add_argument("--benchmark-script",
                        default=str(repo / "qemu_integration" / "qtest_switch_benchmark.py"))
    parser.add_argument("--server", default=str(repo / "build" / "cxlmemsim_server"))
    parser.add_argument("--qemu", default=str(repo / "lib" / "qemu" / "build" / "qemu-system-x86_64"))
    parser.add_argument("--damer-root", default="/root/Damer")
    parser.add_argument("--base-port", type=int, default=10170)
    parser.add_argument("--switch-general-cores", type=int, default=4)
    parser.add_argument("--switch-ai-cores", type=int, default=2)
    parser.add_argument("--switch-general-latency", type=int, default=40)
    parser.add_argument("--switch-ai-latency", type=int, default=30)
    parser.add_argument("--switch-general-bandwidth", type=int, default=64)
    parser.add_argument("--switch-ai-ops-per-ns", type=int, default=128)
    args = parser.parse_args()

    run_dir = Path(args.run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    selected_profiles = profiles(args.preset)
    rows = []
    for index, profile in enumerate(selected_profiles):
        print(
            f"RUN {profile.name}: layers={profile.layers} ranks={profile.ranks} "
            f"dim={profile.dim} kv={profile.kv_shard_bytes} reduce={profile.reduce_count}"
        )
        rows.append(run_one(args, profile, index))

    csv_path, json_path = write_outputs(rows, run_dir, selected_profiles, args)
    print_summary(rows)
    print(f"RESULT_CSV {csv_path}")
    print(f"RESULT_JSON {json_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR {exc}", file=sys.stderr)
        raise
