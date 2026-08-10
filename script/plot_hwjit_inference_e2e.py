#!/usr/bin/env python3
"""Plot Qwen27B inference data-movement edge HW-JIT results."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


CASE_LABELS = {
    "hwjit_qwen27b_kv_pack": "Decode\nKV pack",
    "hwjit_qwen27b_prefill_attention_ffn_e2e": "Prefill + attention\n+ FFN flow",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--csv",
        default="artifact/hwjit_inference_e2e/hwjit_qwen27b_switch_edge.csv",
        help="Input qtest_switch_benchmark CSV.",
    )
    parser.add_argument(
        "--out-dir",
        default="artifact/hwjit_inference_e2e",
        help="Directory for generated figures.",
    )
    return parser.parse_args()


def load_rows(csv_path: Path) -> list[dict[str, str]]:
    with csv_path.open(newline="") as handle:
        rows = [row for row in csv.DictReader(handle) if row.get("status") == "PASS"]
    rows = [row for row in rows if row["case"] in CASE_LABELS]
    rows.sort(key=lambda row: list(CASE_LABELS).index(row["case"]))
    if len(rows) != len(CASE_LABELS):
        found = ", ".join(row["case"] for row in rows)
        raise SystemExit(f"expected {len(CASE_LABELS)} HW-JIT rows, found: {found}")
    return rows


def main() -> None:
    args = parse_args()
    csv_path = Path(args.csv)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(csv_path)
    labels = [CASE_LABELS[row["case"]] for row in rows]
    baseline_us = np.array([float(row["baseline_latency_ns"]) / 1000.0 for row in rows])
    hwjit_us = np.array([float(row["hwjit_latency_ns"]) / 1000.0 for row in rows])
    speedup = np.array([float(row["speedup"]) for row in rows])

    plt.rcParams.update({
        "font.size": 9,
        "axes.labelsize": 9,
        "axes.titlesize": 10,
        "legend.fontsize": 8,
        "xtick.labelsize": 8,
        "ytick.labelsize": 8,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    })

    fig, (ax_latency, ax_speedup) = plt.subplots(
        1, 2, figsize=(6.8, 2.45), gridspec_kw={"width_ratios": [1.35, 1.0]}
    )

    x = np.arange(len(labels))
    width = 0.34
    baseline_color = "#4b5563"
    hwjit_color = "#0f766e"
    speedup_color = "#2563eb"

    ax_latency.bar(x - width / 2, baseline_us, width, label="Host CXL read/compute/write",
                   color=baseline_color)
    ax_latency.bar(x + width / 2, hwjit_us, width, label="CXL switch HW-JIT",
                   color=hwjit_color)
    ax_latency.set_yscale("log")
    ax_latency.set_ylabel("Modeled edge latency (us, log)")
    ax_latency.set_xticks(x, labels)
    ax_latency.grid(axis="y", which="major", linestyle=":", linewidth=0.6, alpha=0.7)
    ax_latency.legend(loc="upper right", frameon=False)
    ax_latency.set_title("Qwen27B inference data movement")

    for xpos, value in zip(x - width / 2, baseline_us):
        ax_latency.text(xpos, value * 1.12, f"{value:.1f}", ha="center", va="bottom", fontsize=7)
    for xpos, value in zip(x + width / 2, hwjit_us):
        ax_latency.text(xpos, value * 1.2, f"{value:.3f}", ha="center", va="bottom", fontsize=7)

    ax_speedup.bar(x, speedup, 0.48, color=speedup_color)
    ax_speedup.set_ylabel("Speedup over host baseline")
    ax_speedup.set_xticks(x, labels)
    ax_speedup.grid(axis="y", linestyle=":", linewidth=0.6, alpha=0.7)
    ax_speedup.set_title("Data-movement edge speedup")
    ax_speedup.set_ylim(0, max(speedup) * 1.22)
    for xpos, value in zip(x, speedup):
        ax_speedup.text(xpos, value + max(speedup) * 0.035, f"{value:.1f}x",
                        ha="center", va="bottom", fontsize=8, fontweight="bold")

    fig.text(
        0.01,
        0.01,
        f"Source: {csv_path}; data-movement edge only, not full-token inference latency.",
        fontsize=6.5,
        color="#374151",
    )
    fig.tight_layout(rect=(0, 0.05, 1, 1))

    for suffix in ("pdf", "png"):
        fig.savefig(out_dir / f"qwen27b_inference_hwjit_e2e.{suffix}", bbox_inches="tight", dpi=300)

    print(f"WROTE {out_dir / 'qwen27b_inference_hwjit_e2e.pdf'}")
    print(f"WROTE {out_dir / 'qwen27b_inference_hwjit_e2e.png'}")


if __name__ == "__main__":
    main()
