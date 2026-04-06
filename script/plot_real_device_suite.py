#!/usr/bin/env python3

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


LABELS = {
    "gromacs": "GROMACS",
    "tigon_tpcc": "Tigon TPCC",
    "mlc_idle": "MLC idle",
    "wrf_short": "WRF-short",
    "gapbs_bfs": "GAPBS BFS",
    "gapbs_pr": "GAPBS PR",
    "gapbs_cc": "GAPBS CC",
    "gapbs_sssp": "GAPBS SSSP",
}


def load_records(summary_csv: Path) -> list[dict[str, str]]:
    with summary_csv.open() as fh:
        return list(csv.DictReader(fh))


def compute_normalized(records: list[dict[str, str]], baseline_node: int, compare_node: int) -> list[dict[str, object]]:
    by_workload: dict[str, dict[int, dict[str, str]]] = {}
    for record in records:
        if record["status"] != "ok":
            continue
        workload = record["workload"]
        node = int(record["node"])
        by_workload.setdefault(workload, {})[node] = record

    normalized: list[dict[str, object]] = []
    for workload, nodes in by_workload.items():
        if baseline_node not in nodes or compare_node not in nodes:
            continue
        baseline = float(nodes[baseline_node]["metric_value"])
        compare = float(nodes[compare_node]["metric_value"])
        higher_is_better = nodes[baseline_node]["higher_is_better"].lower() == "true"
        ratio = compare / baseline if higher_is_better else baseline / compare
        normalized.append(
            {
                "workload": workload,
                "label": LABELS.get(workload, workload),
                "ratio": ratio,
                "baseline": baseline,
                "compare": compare,
                "unit": nodes[baseline_node]["unit"],
                "higher_is_better": higher_is_better,
            }
        )
    return normalized


def plot(normalized: list[dict[str, object]], output_dir: Path, baseline_node: int, compare_node: int) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    labels = [entry["label"] for entry in normalized]
    ratios = np.array([entry["ratio"] for entry in normalized], dtype=float)
    colors = ["#0b6e4f" if ratio >= 1.0 else "#c84c09" for ratio in ratios]

    fig, ax = plt.subplots(figsize=(8, 4), constrained_layout=True)
    bars = ax.bar(labels, ratios, color=colors, width=0.68)
    ax.axhline(1.0, color="#222222", linewidth=1.2, linestyle="--")
    ax.set_ylabel(f"Normalized performance (node{compare_node} vs node{baseline_node})")
    ax.set_title("Real-device benchmark comparison")
    ax.set_ylim(0, max(1.4, ratios.max() * 1.18))
    ax.tick_params(axis="x", rotation=20)

    for bar, entry in zip(bars, normalized, strict=True):
        height = bar.get_height()
        ax.text(
            bar.get_x() + bar.get_width() / 2.0,
            height + 0.02,
            f"{height:.2f}x",
            ha="center",
            va="bottom",
            fontsize=9,
        )
        raw = f"n{baseline_node}={entry['baseline']:.1f} {entry['unit']}\n" f"n{compare_node}={entry['compare']:.1f} {entry['unit']}"
        ax.text(
            bar.get_x() + bar.get_width() / 2.0,
            0.03,
            raw,
            ha="center",
            va="bottom",
            fontsize=7,
            color="#333333",
            rotation=90,
        )

    stem = output_dir / "real_device_normalized_performance"
    fig.savefig(stem.with_suffix(".png"), dpi=220)
    fig.savefig(stem.with_suffix(".pdf"))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Plot normalized real-device benchmark results")
    parser.add_argument("summary_csv", type=Path, help="Path to summary.csv from run_real_device_suite.py")
    parser.add_argument("--baseline-node", type=int, default=1, help="Baseline node for normalization")
    parser.add_argument("--compare-node", type=int, default=2, help="Compared node for normalization")
    parser.add_argument("--out-dir", type=Path, default=None, help="Figure output directory")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    records = load_records(args.summary_csv)
    normalized = compute_normalized(records, args.baseline_node, args.compare_node)
    if not normalized:
        raise SystemExit("No comparable successful records found in summary.csv")
    out_dir = args.out_dir or args.summary_csv.parent / "figures"
    plot(normalized, out_dir, args.baseline_node, args.compare_node)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
