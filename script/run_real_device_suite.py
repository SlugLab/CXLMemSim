#!/usr/bin/env python3

import argparse
import csv
import json
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable


REPO_ROOT = Path(__file__).resolve().parents[1]
ARTIFACT_ROOT = REPO_ROOT / "artifact" / "real_device_suite"
GAPBS_BUILD_DIR = REPO_ROOT / "workloads" / "gapbs" / "build"
WRF_SRC_DIR = REPO_ROOT / "workloads" / "wrf"


@dataclass
class CommandResult:
    exit_code: int
    stdout: str
    wall_seconds: float
    max_rss_kb: int | None
    timed_out: bool


def run_command(
    command: list[str],
    cwd: Path,
    log_path: Path,
    metrics_path: Path,
    timeout_seconds: int | None = None,
    env_overrides: dict[str, str] | None = None,
) -> CommandResult:
    env = os.environ.copy()
    if env_overrides:
        env.update(env_overrides)

    timed_out = False
    start = time.monotonic()
    wrapped = [
        "/usr/bin/time",
        "-f",
        "wall_seconds %e\nmax_rss_kb %M",
        "-o",
        str(metrics_path),
        *command,
    ]
    try:
        proc = subprocess.run(
            wrapped,
            cwd=cwd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout_seconds,
            check=False,
        )
        stdout = proc.stdout
        exit_code = proc.returncode
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or ""
        exit_code = 124
        timed_out = True
    wall_seconds = time.monotonic() - start
    log_path.write_text(stdout)

    max_rss_kb = None
    if metrics_path.exists():
        metrics = parse_simple_summary(metrics_path)
        wall_seconds = float(metrics.get("wall_seconds", wall_seconds))
        if "max_rss_kb" in metrics:
            max_rss_kb = int(float(metrics["max_rss_kb"]))

    return CommandResult(
        exit_code=exit_code,
        stdout=stdout,
        wall_seconds=wall_seconds,
        max_rss_kb=max_rss_kb,
        timed_out=timed_out,
    )


def parse_simple_summary(path: Path) -> dict[str, str]:
    parsed: dict[str, str] = {}
    for raw_line in path.read_text().splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if "=" in line:
            key, value = line.split("=", 1)
            parsed[key.strip()] = value.strip()
            continue
        parts = line.split(None, 1)
        if len(parts) == 2:
            parsed[parts[0].strip()] = parts[1].strip()
    return parsed


def ensure_gapbs_built() -> None:
    bfs_bin = GAPBS_BUILD_DIR / "bfs"
    if bfs_bin.exists():
        return
    GAPBS_BUILD_DIR.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cmake", "-S", ".", "-B", "build"], cwd=REPO_ROOT / "workloads" / "gapbs", check=True)
    subprocess.run(["cmake", "--build", "build", "-j4"], cwd=REPO_ROOT / "workloads" / "gapbs", check=True)


def prepare_wrf_case(case_dir: Path, run_minutes: int) -> None:
    case_dir.mkdir(parents=True, exist_ok=True)
    for src in WRF_SRC_DIR.iterdir():
        dst = case_dir / src.name
        if dst.exists():
            continue
        if src.name == "namelist.input":
            continue
        dst.symlink_to(src)

    namelist_text = (WRF_SRC_DIR / "namelist.input").read_text()
    replacements = {
        r"run_days\s*=\s*1,": "run_days                            = 0,",
        r"run_hours\s*=\s*0,": "run_hours                           = 0,",
        r"run_minutes\s*=\s*0,": f"run_minutes                         = {run_minutes},",
    }
    for pattern, replacement in replacements.items():
        namelist_text = re.sub(pattern, replacement, namelist_text, count=1)
    (case_dir / "namelist.input").write_text(namelist_text)


def parse_gromacs_metric(summary_path: Path) -> tuple[float, str]:
    summary = parse_simple_summary(summary_path)
    return float(summary["ns_per_day"]), "ns/day"


def parse_tigon_metric(summary_path: Path) -> tuple[float, str]:
    summary = parse_simple_summary(summary_path)
    return float(summary["total_commit"]), "commits"


def parse_gapbs_metric(log_path: Path) -> tuple[float, str]:
    text = log_path.read_text()
    match = re.search(r"wall_seconds\s+([0-9.]+)", text)
    if match:
        return float(match.group(1)), "s"
    raise ValueError(f"Missing wall_seconds in {log_path}")


def parse_mlc_idle_latency(log_path: Path) -> tuple[float, str]:
    text = log_path.read_text()
    match = re.search(r"Each iteration took .*?\(\s*([0-9.]+)\s*ns\)", text)
    if not match:
        raise ValueError(f"Missing MLC latency in {log_path}")
    return float(match.group(1)), "ns"


def parse_wall_seconds(metrics_path: Path) -> tuple[float, str]:
    metrics = parse_simple_summary(metrics_path)
    return float(metrics["wall_seconds"]), "s"


def copy_tree_text(src: Path, dst: Path) -> None:
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst)


def run_benchmark_real_node(workload: str, node: int, result_dir: Path) -> CommandResult:
    command = [
        str(REPO_ROOT / "script" / "benchmark_real_node.sh"),
        "--mem-node",
        str(node),
        f"--{workload}-only",
        "--out-dir",
        str(result_dir),
    ]
    return run_command(
        command=command,
        cwd=REPO_ROOT,
        log_path=result_dir / f"{workload}.wrapper.log",
        metrics_path=result_dir / f"{workload}.wrapper.metrics.txt",
        timeout_seconds=600,
    )


def format_command(command: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in command)


def write_record_csv(records: list[dict[str, object]], path: Path) -> None:
    fieldnames = [
        "workload",
        "node",
        "metric_name",
        "metric_value",
        "unit",
        "higher_is_better",
        "status",
        "exit_code",
        "wall_seconds",
        "max_rss_kb",
        "command",
        "note",
    ]
    with path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(records)


def run_suite(args: argparse.Namespace) -> Path:
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    suite_dir = Path(args.out_dir) if args.out_dir else ARTIFACT_ROOT / timestamp
    suite_dir.mkdir(parents=True, exist_ok=True)

    metadata = {
        "hostname": socket.gethostname(),
        "nodes": args.nodes,
        "created_utc": timestamp,
        "workloads": args.workloads,
    }
    (suite_dir / "metadata.json").write_text(json.dumps(metadata, indent=2))

    subprocess.run(
        ["numactl", "--hardware"],
        cwd=REPO_ROOT,
        check=False,
        stdout=(suite_dir / "numactl_hardware.txt").open("w"),
        stderr=subprocess.STDOUT,
        text=True,
    )

    if any(workload.startswith("gapbs_") for workload in args.workloads):
        ensure_gapbs_built()

    records: list[dict[str, object]] = []

    for workload in args.workloads:
        for node in args.nodes:
            workload_dir = suite_dir / f"node{node}" / workload
            workload_dir.mkdir(parents=True, exist_ok=True)
            print(f"[node{node}] {workload}", flush=True)
            log_path = workload_dir / "run.log"
            metrics_path = workload_dir / "metrics.txt"
            note = ""
            status = "ok"
            metric_name = ""
            metric_value: float | None = None
            unit = ""
            higher_is_better = True
            max_rss_kb: int | None = None
            exit_code = 0
            command: list[str] = []

            if workload == "gromacs":
                wrapper = run_benchmark_real_node("gromacs", node, workload_dir)
                summary_path = workload_dir / "gromacs" / "summary.txt"
                summary = parse_simple_summary(summary_path)
                exit_code = wrapper.exit_code
                if "max_rss_kb" in summary:
                    max_rss_kb = int(float(summary["max_rss_kb"]))
                metric_value, unit = parse_gromacs_metric(summary_path)
                metric_name = "ns_per_day"
                higher_is_better = True
                note = "Generated water-box case; metric from GROMACS Performance line."
                command = [
                    str(REPO_ROOT / "script" / "benchmark_real_node.sh"),
                    "--mem-node",
                    str(node),
                    "--gromacs-only",
                    "--out-dir",
                    str(workload_dir),
                ]
                log_path = workload_dir / "gromacs" / "mdrun.log"
                metrics_path = workload_dir / "gromacs" / "metrics.txt"
            elif workload == "tigon_tpcc":
                wrapper = run_benchmark_real_node("tigon", node, workload_dir)
                summary_path = workload_dir / "tigon" / "summary.txt"
                summary = parse_simple_summary(summary_path)
                exit_code = int(summary.get("exit_code", wrapper.exit_code))
                if "max_rss_kb" in summary:
                    max_rss_kb = int(float(summary["max_rss_kb"]))
                metric_value, unit = parse_tigon_metric(summary_path)
                metric_name = "total_commit"
                higher_is_better = True
                note = "Tigon produces final stats before a known teardown SIGSEGV."
                command = [
                    str(REPO_ROOT / "script" / "benchmark_real_node.sh"),
                    "--mem-node",
                    str(node),
                    "--tigon-only",
                    "--out-dir",
                    str(workload_dir),
                ]
                log_path = workload_dir / "tigon" / "run.log"
                metrics_path = workload_dir / "tigon" / "metrics.txt"
            elif workload == "mlc_idle":
                command = [
                    "numactl",
                    "-m",
                    str(node),
                    str(REPO_ROOT / "workloads" / "MLC" / "mlc"),
                    "--idle_latency",
                    f"-j{node}",
                    "-r",
                    "-t1",
                    "-b256000",
                ]
                result = run_command(
                    command=command,
                    cwd=REPO_ROOT / "workloads" / "MLC",
                    log_path=log_path,
                    metrics_path=metrics_path,
                    timeout_seconds=30,
                )
                exit_code = result.exit_code
                max_rss_kb = result.max_rss_kb
                metric_value, unit = parse_mlc_idle_latency(log_path)
                metric_name = "idle_latency_ns"
                higher_is_better = False
                note = "250 MiB random idle-latency probe to keep node2 runtime bounded."
            elif workload == "wrf_short":
                case_dir = workload_dir / "case"
                prepare_wrf_case(case_dir, args.wrf_run_minutes)
                command = ["env", "LD_LIBRARY_PATH=.", "numactl", "-m", str(node), "./wrf.exe"]
                result = run_command(
                    command=command,
                    cwd=case_dir,
                    log_path=log_path,
                    metrics_path=metrics_path,
                    timeout_seconds=180,
                )
                exit_code = result.exit_code
                max_rss_kb = result.max_rss_kb
                metric_value, unit = parse_wall_seconds(metrics_path)
                metric_name = "runtime_seconds"
                higher_is_better = False
                note = f"Reduced WRF case with run_minutes={args.wrf_run_minutes}."
            elif workload.startswith("gapbs_"):
                algorithm = workload.split("_", 1)[1]
                command = [
                    "numactl",
                    "-m",
                    str(node),
                    str(GAPBS_BUILD_DIR / algorithm),
                    "-g",
                    str(args.gapbs_scale),
                    "-n",
                    "1",
                    "-r",
                    "0",
                ]
                result = run_command(
                    command=command,
                    cwd=REPO_ROOT / "workloads" / "gapbs",
                    log_path=log_path,
                    metrics_path=metrics_path,
                    timeout_seconds=90,
                )
                exit_code = result.exit_code
                max_rss_kb = result.max_rss_kb
                metric_value, unit = parse_wall_seconds(metrics_path)
                metric_name = "runtime_seconds"
                higher_is_better = False
                note = f"Kronecker graph scale={args.gapbs_scale}, 1 trial, fixed source."
            else:
                raise ValueError(f"Unknown workload: {workload}")

            if exit_code != 0 and workload != "tigon_tpcc":
                status = "failed"
                note = (note + " " if note else "") + f"exit_code={exit_code}"
            elif exit_code != 0 and workload == "tigon_tpcc":
                note = (note + " " if note else "") + f"exit_code={exit_code}"

            metrics = parse_simple_summary(metrics_path) if metrics_path.exists() else {}
            wall_seconds = float(metrics.get("wall_seconds", 0.0))
            records.append(
                {
                    "workload": workload,
                    "node": node,
                    "metric_name": metric_name,
                    "metric_value": metric_value,
                    "unit": unit,
                    "higher_is_better": higher_is_better,
                    "status": status,
                    "exit_code": exit_code,
                    "wall_seconds": wall_seconds,
                    "max_rss_kb": max_rss_kb,
                    "command": format_command(command),
                    "note": note,
                }
            )

    write_record_csv(records, suite_dir / "summary.csv")
    (suite_dir / "summary.json").write_text(json.dumps(records, indent=2))
    return suite_dir


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the real-device benchmark suite with numactl -m")
    parser.add_argument(
        "--nodes",
        type=int,
        nargs="+",
        default=[1, 2],
        help="NUMA memory nodes to benchmark (default: 1 2)",
    )
    parser.add_argument(
        "--workloads",
        nargs="+",
        default=[
            "gromacs",
            "tigon_tpcc",
            "mlc_idle",
            "wrf_short",
            "gapbs_bfs",
            "gapbs_pr",
            "gapbs_cc",
            "gapbs_sssp",
        ],
        help="Workload IDs to run",
    )
    parser.add_argument("--gapbs-scale", type=int, default=18, help="GAPBS Kronecker graph scale")
    parser.add_argument("--wrf-run-minutes", type=int, default=10, help="Reduced WRF run length in minutes")
    parser.add_argument("--out-dir", type=str, default="", help="Output directory")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    suite_dir = run_suite(args)
    print(suite_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
