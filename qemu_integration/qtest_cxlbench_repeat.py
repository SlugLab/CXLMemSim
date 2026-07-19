#!/usr/bin/env python3
"""Run independent repeats for the CXL near-switch benchmark suite."""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path


FIELD_ORDER = [
    "case",
    "usecase",
    "trials",
    "commands_mean",
    "latency_mean_ns",
    "latency_min_ns",
    "latency_max_ns",
    "queued_mean_ns",
    "service_mean_ns",
    "general_ops_mean",
    "ai_ops_mean",
    "bytes_mean",
    "work_items_mean",
    "result",
    "status",
    "damer_trace",
]


def run_trial(args: argparse.Namespace, trial: int) -> list[dict[str, str]]:
    trial_dir = (Path(args.run_dir).resolve() / f"trial_{trial}")
    trial_dir.mkdir(parents=True, exist_ok=True)
    output_prefix = "cxl_switch_benchmark"
    cmd = [
        sys.executable,
        args.benchmark_script,
        f"--port={args.base_port + trial}",
        f"--run-dir={trial_dir}",
        f"--server={args.server}",
        f"--qemu={args.qemu}",
        f"--damer-root={args.damer_root}",
        f"--cases={args.cases}",
        "--repeat=1",
        f"--output-prefix={output_prefix}",
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
        raise RuntimeError(f"CXL benchmark trial {trial} failed; see {log_path}")

    csv_path = trial_dir / f"{output_prefix}.csv"
    with open(csv_path, "r", encoding="utf-8", newline="") as fh:
        return list(csv.DictReader(fh))


def as_int(row: dict[str, str], field: str) -> int:
    return int(row.get(field, "0") or "0")


def mean_int(values: list[int]) -> int:
    return sum(values) // len(values)


def aggregate(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    by_case: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        by_case.setdefault(row["case"], []).append(row)

    out = []
    for case, case_rows in sorted(by_case.items()):
        latencies = [as_int(row, "modeled_latency_ns") for row in case_rows]
        first = case_rows[0]
        out.append({
            "case": case,
            "usecase": first.get("usecase", ""),
            "trials": str(len(case_rows)),
            "commands_mean": str(mean_int([as_int(row, "commands") for row in case_rows])),
            "latency_mean_ns": str(mean_int(latencies)),
            "latency_min_ns": str(min(latencies)),
            "latency_max_ns": str(max(latencies)),
            "queued_mean_ns": str(mean_int([as_int(row, "queued_ns") for row in case_rows])),
            "service_mean_ns": str(mean_int([as_int(row, "service_ns") for row in case_rows])),
            "general_ops_mean": str(mean_int([as_int(row, "general_ops") for row in case_rows])),
            "ai_ops_mean": str(mean_int([as_int(row, "ai_ops") for row in case_rows])),
            "bytes_mean": str(mean_int([as_int(row, "bytes") for row in case_rows])),
            "work_items_mean": str(mean_int([as_int(row, "work_items") for row in case_rows])),
            "result": first.get("result", ""),
            "status": "PASS" if all(row.get("status") == "PASS" for row in case_rows) else "MIXED",
            "damer_trace": first.get("damer_trace", ""),
        })
    return out


def write_outputs(rows: list[dict[str, str]], raw_rows: list[dict[str, str]],
                  args: argparse.Namespace) -> tuple[Path, Path]:
    run_dir = Path(args.run_dir).resolve()
    csv_path = run_dir / "cxl_switch_benchmark_summary.csv"
    json_path = run_dir / "cxl_switch_benchmark_summary.json"
    with open(csv_path, "w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=FIELD_ORDER)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in FIELD_ORDER})

    payload = {
        "config": {
            "trials": args.trials,
            "quick": args.quick,
            "cases": args.cases,
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
        "summary": rows,
        "raw_results": raw_rows,
    }
    with open(json_path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2)
    return csv_path, json_path


def print_summary(rows: list[dict[str, str]]) -> None:
    print("\nCXL SWITCH BENCHMARK SUMMARY")
    print("case,trials,latency_mean_ns,latency_min_ns,latency_max_ns,general_ops,ai_ops,bytes")
    for row in rows:
        print(
            f"{row['case']},{row['trials']},{row['latency_mean_ns']},"
            f"{row['latency_min_ns']},{row['latency_max_ns']},"
            f"{row['general_ops_mean']},{row['ai_ops_mean']},{row['bytes_mean']}"
        )


def main() -> int:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--run-dir", default=str(repo / "build" / "qtest-switch-cxlbench-repeat"))
    parser.add_argument("--benchmark-script",
                        default=str(repo / "qemu_integration" / "qtest_switch_benchmark.py"))
    parser.add_argument("--server", default=str(repo / "build" / "cxlmemsim_server"))
    parser.add_argument("--qemu", default=str(repo / "lib" / "qemu" / "build" / "qemu-system-x86_64"))
    parser.add_argument("--damer-root", default="/root/Damer")
    parser.add_argument("--base-port", type=int, default=10230)
    parser.add_argument("--cases", default="all")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--switch-general-cores", type=int, default=4)
    parser.add_argument("--switch-ai-cores", type=int, default=2)
    parser.add_argument("--switch-general-latency", type=int, default=40)
    parser.add_argument("--switch-ai-latency", type=int, default=30)
    parser.add_argument("--switch-general-bandwidth", type=int, default=64)
    parser.add_argument("--switch-ai-ops-per-ns", type=int, default=128)
    args = parser.parse_args()

    if args.trials < 1:
        raise RuntimeError("--trials must be at least 1")

    Path(args.run_dir).resolve().mkdir(parents=True, exist_ok=True)
    raw_rows = []
    for trial in range(args.trials):
        print(f"RUN trial {trial}")
        rows = run_trial(args, trial)
        for row in rows:
            row["trial"] = str(trial)
        raw_rows.extend(rows)

    summary_rows = aggregate(raw_rows)
    csv_path, json_path = write_outputs(summary_rows, raw_rows, args)
    print_summary(summary_rows)
    print(f"RESULT_CSV {csv_path}")
    print(f"RESULT_JSON {json_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR {exc}", file=sys.stderr)
        raise
