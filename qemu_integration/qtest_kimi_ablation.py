#!/usr/bin/env python3
"""Run Kimi collective ablations on the emulated CXL switch."""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class AblationConfig:
    name: str
    allgather_mode: str
    allreduce_mode: str


FIELD_ORDER = [
    "ablation",
    "allgather_mode",
    "allreduce_mode",
    "layers",
    "ranks",
    "dim",
    "kv_shard_bytes",
    "reduce_count",
    "case",
    "commands",
    "command_sequence",
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


def ablation_configs() -> list[AblationConfig]:
    return [
        AblationConfig("full_collectives", "full", "full"),
        AblationConfig("single_destination_allgather", "single-dst", "full"),
        AblationConfig("reduce_only_allreduce", "full", "reduce-only"),
        AblationConfig("minimal_collectives", "single-dst", "reduce-only"),
    ]


def run_one(args: argparse.Namespace, cfg: AblationConfig, index: int) -> dict[str, str]:
    output_prefix = "kimi_ablation"
    run_dir = (Path(args.run_dir) / cfg.name).resolve()
    run_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    for trial in range(args.repeat):
        trial_dir = run_dir / f"trial_{trial}"
        trial_dir.mkdir(parents=True, exist_ok=True)
        cmd = [
            sys.executable,
            args.benchmark_script,
            f"--port={args.base_port + index * args.repeat + trial}",
            f"--run-dir={trial_dir}",
            f"--server={args.server}",
            f"--qemu={args.qemu}",
            f"--damer-root={args.damer_root}",
            "--cases=kimi26_ternary_nccl_hook_e2e",
            "--repeat=1",
            f"--output-prefix={output_prefix}",
            f"--kimi-layers={args.kimi_layers}",
            f"--kimi-ranks={args.kimi_ranks}",
            f"--kimi-dim={args.kimi_dim}",
            f"--kimi-kv-shard-bytes={args.kimi_kv_shard_bytes}",
            f"--kimi-reduce-count={args.kimi_reduce_count}",
            f"--kimi-allgather-mode={cfg.allgather_mode}",
            f"--kimi-allreduce-mode={cfg.allreduce_mode}",
            f"--switch-general-cores={args.switch_general_cores}",
            f"--switch-ai-cores={args.switch_ai_cores}",
            f"--switch-general-latency={args.switch_general_latency}",
            f"--switch-ai-latency={args.switch_ai_latency}",
            f"--switch-general-bandwidth={args.switch_general_bandwidth}",
            f"--switch-ai-ops-per-ns={args.switch_ai_ops_per_ns}",
        ]
        if args.quick:
            cmd.append("--quick")

        proc = subprocess.run(cmd, cwd=Path(args.benchmark_script).parent,
                              text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        log_path = trial_dir / "driver.log"
        with open(log_path, "w", encoding="utf-8") as fh:
            fh.write("$ " + " ".join(cmd) + "\n")
            fh.write(proc.stdout)
            if proc.stderr:
                fh.write("\nSTDERR\n")
                fh.write(proc.stderr)
        if proc.returncode != 0:
            print(proc.stdout, end="")
            print(proc.stderr, end="", file=sys.stderr)
            raise RuntimeError(f"ablation {cfg.name} trial {trial} failed; see {log_path}")

        csv_path = trial_dir / f"{output_prefix}.csv"
        with open(csv_path, "r", encoding="utf-8", newline="") as fh:
            trial_rows = list(csv.DictReader(fh))
        if len(trial_rows) != 1:
            raise RuntimeError(f"ablation {cfg.name} trial {trial} returned {len(trial_rows)} rows")
        rows.append(trial_rows[0])

    row = rows[0] if args.repeat == 1 else aggregate_repeat_rows(rows)
    row.update({
        "ablation": cfg.name,
        "allgather_mode": cfg.allgather_mode,
        "allreduce_mode": cfg.allreduce_mode,
        "layers": str(args.kimi_layers),
        "ranks": str(args.kimi_ranks),
        "dim": str(args.kimi_dim),
        "kv_shard_bytes": str(args.kimi_kv_shard_bytes),
        "reduce_count": str(args.kimi_reduce_count),
    })
    return row


def aggregate_repeat_rows(rows: list[dict[str, str]]) -> dict[str, str]:
    numeric_fields = [
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
    ]
    out = dict(rows[0])
    for field in numeric_fields:
        values = [int(row.get(field, "0") or "0") for row in rows]
        out[field] = str(sum(values) // len(values))
    out["iteration"] = "mean_repeat"
    return out


def write_outputs(rows: list[dict[str, str]], run_dir: Path,
                  configs: list[AblationConfig],
                  args: argparse.Namespace) -> tuple[Path, Path]:
    csv_path = run_dir / "kimi_ablation.csv"
    json_path = run_dir / "kimi_ablation.json"
    with open(csv_path, "w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=FIELD_ORDER)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in FIELD_ORDER})

    payload = {
        "config": {
            "quick": args.quick,
            "repeat": args.repeat,
            "server": args.server,
            "qemu": args.qemu,
            "damer_root": args.damer_root,
            "kimi_layers": args.kimi_layers,
            "kimi_ranks": args.kimi_ranks,
            "kimi_dim": args.kimi_dim,
            "kimi_kv_shard_bytes": args.kimi_kv_shard_bytes,
            "kimi_reduce_count": args.kimi_reduce_count,
            "switch_general_cores": args.switch_general_cores,
            "switch_ai_cores": args.switch_ai_cores,
            "switch_general_latency": args.switch_general_latency,
            "switch_ai_latency": args.switch_ai_latency,
            "switch_general_bandwidth": args.switch_general_bandwidth,
            "switch_ai_ops_per_ns": args.switch_ai_ops_per_ns,
        },
        "ablations": [cfg.__dict__ for cfg in configs],
        "results": rows,
    }
    with open(json_path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2)
    return csv_path, json_path


def as_int(row: dict[str, str], key: str) -> int:
    return int(row.get(key, "0") or "0")


def print_summary(rows: list[dict[str, str]]) -> None:
    baseline = next((row for row in rows if row["ablation"] == "full_collectives"), None)
    baseline_latency = as_int(baseline, "modeled_latency_ns") if baseline else 0

    print("\nKIMI COLLECTIVE ABLATION SUMMARY")
    print("ablation,allgather,allreduce,commands,latency_ns,delta_vs_full_pct,general_ops,ai_ops,bytes")
    for row in rows:
        latency = as_int(row, "modeled_latency_ns")
        if baseline_latency > 0:
            delta = 100.0 * (latency - baseline_latency) / baseline_latency
            delta_text = f"{delta:.2f}"
        else:
            delta_text = "nan"
        print(
            f"{row['ablation']},{row['allgather_mode']},{row['allreduce_mode']},"
            f"{row['commands']},{latency},{delta_text},"
            f"{row['general_ops']},{row['ai_ops']},{row['bytes']}"
        )


def main() -> int:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", default=str(repo / "build" / "qtest-switch-kimi26-ablation"))
    parser.add_argument("--benchmark-script",
                        default=str(repo / "qemu_integration" / "qtest_switch_benchmark.py"))
    parser.add_argument("--server", default=str(repo / "build" / "cxlmemsim_server"))
    parser.add_argument("--qemu", default=str(repo / "lib" / "qemu" / "build" / "qemu-system-x86_64"))
    parser.add_argument("--damer-root", default="/root/Damer")
    parser.add_argument("--base-port", type=int, default=10190)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--kimi-layers", type=int, default=3)
    parser.add_argument("--kimi-ranks", type=int, default=4)
    parser.add_argument("--kimi-dim", type=int, default=8)
    parser.add_argument("--kimi-kv-shard-bytes", type=int, default=256)
    parser.add_argument("--kimi-reduce-count", type=int, default=64)
    parser.add_argument("--switch-general-cores", type=int, default=4)
    parser.add_argument("--switch-ai-cores", type=int, default=2)
    parser.add_argument("--switch-general-latency", type=int, default=40)
    parser.add_argument("--switch-ai-latency", type=int, default=30)
    parser.add_argument("--switch-general-bandwidth", type=int, default=64)
    parser.add_argument("--switch-ai-ops-per-ns", type=int, default=128)
    args = parser.parse_args()

    if args.repeat < 1:
        raise RuntimeError("--repeat must be at least 1")

    run_dir = Path(args.run_dir).resolve()
    run_dir.mkdir(parents=True, exist_ok=True)
    configs = ablation_configs()
    rows = []
    for index, cfg in enumerate(configs):
        print(
            f"RUN {cfg.name}: allgather={cfg.allgather_mode} "
            f"allreduce={cfg.allreduce_mode}"
        )
        rows.append(run_one(args, cfg, index))

    csv_path, json_path = write_outputs(rows, run_dir, configs, args)
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
