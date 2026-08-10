#!/usr/bin/env python3
"""Plot HW-JIT size scaling and full-inference speedup bounds."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--size-sweep",
        default="artifact/hwjit_inference_e2e/size_sweep_summary.csv",
        help="CSV with size_bytes,baseline_ns,hwjit_ns,speedup.",
    )
    parser.add_argument(
        "--e2e-csv",
        default="artifact/hwjit_inference_e2e/hwjit_qwen27b_switch_edge.csv",
        help="Focused Qwen27B HW-JIT benchmark CSV.",
    )
    parser.add_argument(
        "--out-dir",
        default="artifact/hwjit_inference_e2e",
        help="Directory for generated figures.",
    )
    return parser.parse_args()


def read_size_sweep(path: Path) -> tuple[np.ndarray, np.ndarray]:
    sizes: list[float] = []
    speedups: list[float] = []
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row.get("status") != "PASS":
                continue
            sizes.append(float(row["size_bytes"]))
            speedups.append(float(row["speedup"]))
    if not sizes:
        raise SystemExit(f"no passing rows in {path}")
    return np.array(sizes), np.array(speedups)


def read_e2e_speedups(path: Path) -> dict[str, float]:
    speedups: dict[str, float] = {}
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row.get("status") != "PASS":
                continue
            speedups[row["case"]] = float(row["speedup"])
    required = {
        "hwjit_qwen27b_kv_pack": "Decode KV pack",
        "hwjit_qwen27b_prefill_attention_ffn_e2e": "Prefill/attention/FFN flow",
    }
    missing = [case for case in required if case not in speedups]
    if missing:
        raise SystemExit(f"missing focused HW-JIT rows: {missing}")
    return {required[case]: speedups[case] for case in required}


def amdahl(data_fraction: np.ndarray, data_speedup: float) -> np.ndarray:
    return 1.0 / ((1.0 - data_fraction) + data_fraction / data_speedup)


def main() -> None:
    args = parse_args()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    sizes, size_speedups = read_size_sweep(Path(args.size_sweep))
    e2e_speedups = read_e2e_speedups(Path(args.e2e_csv))

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

    fig, (ax_size, ax_bound) = plt.subplots(1, 2, figsize=(7.2, 2.6))

    ax_size.plot(sizes, size_speedups, marker="o", linewidth=1.8, color="#0f766e")
    ax_size.set_xscale("log", base=2)
    ax_size.set_xlabel("KV pack payload size (bytes)")
    ax_size.set_ylabel("Data-movement speedup")
    ax_size.set_title("Not only a small-packet effect")
    ax_size.grid(True, which="major", linestyle=":", linewidth=0.6, alpha=0.7)
    for size, speedup in zip(sizes, size_speedups):
        if size in (64, 1024, 8192, 1048576):
            ax_size.text(size, speedup * 1.05, f"{speedup:.0f}x",
                         ha="center", va="bottom", fontsize=7)

    fractions = np.linspace(0.0, 0.7, 200)
    colors = ["#2563eb", "#7c3aed"]
    for (label, speedup), color in zip(e2e_speedups.items(), colors):
        ax_bound.plot(fractions * 100.0, amdahl(fractions, speedup),
                      label=f"{label} edge ({speedup:.1f}x)", linewidth=1.8,
                      color=color)
    markers = [0.05, 0.10, 0.20, 0.40]
    for marker in markers:
        ax_bound.axvline(marker * 100.0, color="#9ca3af", linewidth=0.6,
                         linestyle=":", zorder=0)
    ax_bound.set_xlabel("Fabric movement share (%)")
    ax_bound.set_ylabel("Full-token speedup bound")
    ax_bound.set_title("Full inference gain is Amdahl-limited")
    ax_bound.set_ylim(1.0, 2.7)
    ax_bound.grid(axis="y", linestyle=":", linewidth=0.6, alpha=0.7)
    ax_bound.legend(loc="upper left", frameon=False, handlelength=2.4)

    fig.tight_layout()
    for suffix in ("pdf", "png"):
        fig.savefig(out_dir / f"qwen27b_hwjit_size_and_e2e_bound.{suffix}",
                    bbox_inches="tight", dpi=300)

    print(f"WROTE {out_dir / 'qwen27b_hwjit_size_and_e2e_bound.pdf'}")
    print(f"WROTE {out_dir / 'qwen27b_hwjit_size_and_e2e_bound.png'}")


if __name__ == "__main__":
    main()
