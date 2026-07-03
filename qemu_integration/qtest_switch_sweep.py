#!/usr/bin/env python3
"""Run configuration sweeps for QEMU Type2 near-switch offload benchmarks."""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class SweepConfig:
    name: str
    general_cores: int
    ai_cores: int
    general_latency: int
    ai_latency: int
    general_bandwidth: int
    ai_ops_per_ns: int


FIELD_ORDER = [
    "sweep",
    "general_cores_config",
    "ai_cores_config",
    "general_latency_ns_config",
    "ai_latency_ns_config",
    "general_bandwidth_bytes_per_ns_config",
    "ai_ops_per_ns_config",
    "iteration",
    "case",
    "usecase",
    "damer_workload",
    "damer_events",
    "commands",
    "command_sequence",
    "bytes",
    "work_items",
    "modeled_latency_ns",
    "general_ops",
    "ai_ops",
    "general_bytes",
    "ai_bytes",
    "queued_ns",
    "service_ns",
    "result",
    "status",
    "damer_trace",
]


def sweep_configs(preset: str) -> list[SweepConfig]:
    balanced = SweepConfig("balanced", 4, 2, 40, 30, 64, 128)
    core_scale = [
        SweepConfig("single_core", 1, 1, 40, 30, 64, 128),
        SweepConfig("general_core_scale", 8, 2, 40, 30, 64, 128),
        SweepConfig("ai_core_scale", 4, 4, 40, 30, 64, 128),
    ]
    rate_sweep = [
        SweepConfig("slow_general_bandwidth", 4, 2, 40, 30, 16, 128),
        SweepConfig("fast_general_bandwidth", 4, 2, 40, 30, 256, 128),
        SweepConfig("slow_ai_rate", 4, 2, 40, 30, 64, 32),
        SweepConfig("fast_ai_rate", 4, 2, 40, 30, 64, 512),
        SweepConfig("high_base_latency", 4, 2, 120, 90, 64, 128),
    ]
    if preset == "balanced":
        return [balanced]
    if preset == "core-scale":
        return [balanced] + core_scale
    if preset == "rates":
        return [balanced] + rate_sweep
    if preset == "all":
        return [balanced] + core_scale + rate_sweep
    raise RuntimeError(f"unknown preset {preset!r}")


def run_one(args: argparse.Namespace, cfg: SweepConfig, index: int) -> list[dict[str, str]]:
    run_dir = Path(args.run_dir) / cfg.name
    run_dir.mkdir(parents=True, exist_ok=True)
    output_prefix = "switch_benchmark"
    cmd = [
        sys.executable,
        args.benchmark_script,
        f"--port={args.base_port + index}",
        f"--run-dir={run_dir}",
        f"--server={args.server}",
        f"--qemu={args.qemu}",
        f"--damer-root={args.damer_root}",
        f"--cases={args.cases}",
        f"--repeat={args.repeat}",
        f"--output-prefix={output_prefix}",
        f"--switch-general-cores={cfg.general_cores}",
        f"--switch-ai-cores={cfg.ai_cores}",
        f"--switch-general-latency={cfg.general_latency}",
        f"--switch-ai-latency={cfg.ai_latency}",
        f"--switch-general-bandwidth={cfg.general_bandwidth}",
        f"--switch-ai-ops-per-ns={cfg.ai_ops_per_ns}",
    ]
    if args.quick:
        cmd.append("--quick")

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
        raise RuntimeError(f"sweep {cfg.name} failed; see {log_path}")

    csv_path = run_dir / f"{output_prefix}.csv"
    with open(csv_path, "r", encoding="utf-8", newline="") as fh:
        rows = list(csv.DictReader(fh))

    enriched = []
    for row in rows:
        row.update({
            "sweep": cfg.name,
            "general_cores_config": str(cfg.general_cores),
            "ai_cores_config": str(cfg.ai_cores),
            "general_latency_ns_config": str(cfg.general_latency),
            "ai_latency_ns_config": str(cfg.ai_latency),
            "general_bandwidth_bytes_per_ns_config": str(cfg.general_bandwidth),
            "ai_ops_per_ns_config": str(cfg.ai_ops_per_ns),
        })
        enriched.append(row)
    return enriched


def write_aggregate(rows: list[dict[str, str]], run_dir: Path,
                    configs: list[SweepConfig], args: argparse.Namespace) -> tuple[Path, Path]:
    csv_path = run_dir / "switch_sweep.csv"
    json_path = run_dir / "switch_sweep.json"
    with open(csv_path, "w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=FIELD_ORDER)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in FIELD_ORDER})

    payload = {
        "config": {
            "preset": args.preset,
            "quick": args.quick,
            "repeat": args.repeat,
            "cases": args.cases,
            "base_port": args.base_port,
            "benchmark_script": args.benchmark_script,
            "server": args.server,
            "qemu": args.qemu,
            "damer_root": args.damer_root,
        },
        "sweeps": [cfg.__dict__ for cfg in configs],
        "results": rows,
    }
    with open(json_path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2)
    return csv_path, json_path


def as_int(row: dict[str, str], key: str) -> int:
    return int(row.get(key, "0") or "0")


def print_summary(rows: list[dict[str, str]], configs: list[SweepConfig]) -> None:
    print("\nSWEEP SUMMARY")
    print("sweep,total_latency_ns,total_queued_ns,total_service_ns,general_ops,ai_ops")
    by_sweep: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        by_sweep.setdefault(row["sweep"], []).append(row)
    for cfg in configs:
        subset = by_sweep.get(cfg.name, [])
        print(
            f"{cfg.name},"
            f"{sum(as_int(row, 'modeled_latency_ns') for row in subset)},"
            f"{sum(as_int(row, 'queued_ns') for row in subset)},"
            f"{sum(as_int(row, 'service_ns') for row in subset)},"
            f"{sum(as_int(row, 'general_ops') for row in subset)},"
            f"{sum(as_int(row, 'ai_ops') for row in subset)}"
        )

    print("\nCASE LATENCY BY SWEEP")
    print("case," + ",".join(cfg.name for cfg in configs))
    cases = sorted({row["case"] for row in rows})
    for case in cases:
        values = []
        for cfg in configs:
            latency = sum(
                as_int(row, "modeled_latency_ns")
                for row in rows
                if row["sweep"] == cfg.name and row["case"] == case
            )
            values.append(str(latency))
        print(f"{case}," + ",".join(values))


def main() -> int:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--preset", choices=["balanced", "core-scale", "rates", "all"],
                        default="all")
    parser.add_argument("--run-dir", default=str(repo / "build" / "qtest-switch-sweep"))
    parser.add_argument("--benchmark-script",
                        default=str(repo / "qemu_integration" / "qtest_switch_benchmark.py"))
    parser.add_argument("--server", default=str(repo / "build" / "cxlmemsim_server"))
    parser.add_argument("--qemu", default=str(repo / "lib" / "qemu" / "build" / "qemu-system-x86_64"))
    parser.add_argument("--damer-root", default="/root/Damer")
    parser.add_argument("--base-port", type=int, default=10140)
    parser.add_argument("--cases", default="all")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()

    if args.repeat < 1:
        raise RuntimeError("--repeat must be at least 1")

    run_dir = Path(args.run_dir)
    run_dir.mkdir(parents=True, exist_ok=True)
    configs = sweep_configs(args.preset)
    rows: list[dict[str, str]] = []
    for index, cfg in enumerate(configs):
        print(
            f"RUN {cfg.name}: general={cfg.general_cores}@{cfg.general_bandwidth}B/ns "
            f"ai={cfg.ai_cores}@{cfg.ai_ops_per_ns}ops/ns "
            f"latency=({cfg.general_latency},{cfg.ai_latency})ns"
        )
        rows.extend(run_one(args, cfg, index))

    csv_path, json_path = write_aggregate(rows, run_dir, configs, args)
    print_summary(rows, configs)
    print(f"RESULT_CSV {csv_path}")
    print(f"RESULT_JSON {json_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR {exc}", file=sys.stderr)
        raise
