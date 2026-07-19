#!/usr/bin/env python3
"""Plot Kimi collective ablation results with the MEMU paper palette."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


# Palette matched to the MEMU PDF architecture figure:
# light layer fills with stronger outlines/accent colors.
C_BLUE_FILL = "#DBEAFE"
C_BLUE_EDGE = "#2176AE"
C_ORANGE_FILL = "#FFE5BF"
C_ORANGE_EDGE = "#F4A261"
C_PURPLE_FILL = "#E9DDF5"
C_PURPLE_EDGE = "#8B5CF6"
C_GREEN_FILL = "#DDEFD9"
C_GREEN_EDGE = "#57A773"
C_BASE = "#64748B"    # slate
C_GRID = "#D9DEE8"
C_TEXT = "#1F2430"


LABELS = {
    "full_collectives": "Full\ncollectives",
    "single_destination_allgather": "Single-dst\nAllGather",
    "reduce_only_allreduce": "Reduce-only\nAllReduce",
    "minimal_collectives": "Minimal\ncollectives",
}

FILLS = {
    "full_collectives": C_BLUE_FILL,
    "single_destination_allgather": C_ORANGE_FILL,
    "reduce_only_allreduce": C_PURPLE_FILL,
    "minimal_collectives": C_GREEN_FILL,
}

EDGES = {
    "full_collectives": C_BLUE_EDGE,
    "single_destination_allgather": C_ORANGE_EDGE,
    "reduce_only_allreduce": C_PURPLE_EDGE,
    "minimal_collectives": C_GREEN_EDGE,
}


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as fh:
        rows = list(csv.DictReader(fh))
    if not rows:
        raise RuntimeError(f"no rows found in {path}")
    return rows


def plot(rows: list[dict[str, str]], out_prefix: Path) -> None:
    order = [
        "full_collectives",
        "single_destination_allgather",
        "reduce_only_allreduce",
        "minimal_collectives",
    ]
    by_name = {row["ablation"]: row for row in rows}
    missing = [name for name in order if name not in by_name]
    if missing:
        raise RuntimeError(f"missing ablation rows: {missing}")

    latencies = np.array([int(by_name[name]["modeled_latency_ns"]) for name in order], dtype=float)
    commands = np.array([int(by_name[name]["commands"]) for name in order], dtype=int)
    baseline = latencies[0]
    normalized = latencies / baseline
    reductions = (1.0 - normalized) * 100.0

    plt.rcParams.update({
        "font.size": 9,
        "axes.labelsize": 9,
        "xtick.labelsize": 8,
        "ytick.labelsize": 8,
        "legend.fontsize": 8,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
        "axes.axisbelow": True,
    })

    fig, ax = plt.subplots(figsize=(4.8, 2.65))
    x = np.arange(len(order))
    bars = ax.bar(
        x,
        normalized,
        width=0.64,
        color=[FILLS[name] for name in order],
        edgecolor=[EDGES[name] for name in order],
        linewidth=1.2,
    )

    ax.axhline(1.0, color=C_BASE, linestyle="--", linewidth=1.0, alpha=0.7)
    ax.set_ylabel("Normalized latency\n(Full = 1.0)")
    ax.set_ylim(0, 1.16)
    ax.set_xticks(x)
    ax.set_xticklabels([LABELS[name] for name in order])
    ax.grid(True, axis="y", color=C_GRID, linewidth=0.8, alpha=0.8)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color("#8A93A5")
    ax.spines["bottom"].set_color("#8A93A5")
    ax.tick_params(colors=C_TEXT)

    for idx, bar in enumerate(bars):
        height = bar.get_height()
        latency_text = f"{latencies[idx] / 1000:.1f}us"
        if idx == 0:
            label = f"{latency_text}\n{commands[idx]} ops"
        else:
            label = f"-{reductions[idx]:.1f}%\n{latency_text}"
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            height + 0.035,
            label,
            ha="center",
            va="bottom",
            fontsize=7.5,
            color=C_TEXT,
            linespacing=1.05,
        )

    ax.text(
        0.99,
        0.98,
        "Lower is better",
        transform=ax.transAxes,
        ha="right",
        va="top",
        fontsize=8,
        color=C_BASE,
    )

    fig.tight_layout(pad=0.8)
    out_prefix.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_prefix.with_suffix(".pdf"), bbox_inches="tight")
    fig.savefig(out_prefix.with_suffix(".png"), dpi=300, bbox_inches="tight")
    plt.close(fig)


def main() -> None:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--input",
        default=str(repo / "build" / "qtest-switch-kimi-ablation-final" / "kimi_ablation.csv"),
    )
    parser.add_argument(
        "--out-prefix",
        default=str(repo / "artifact" / "splash_sweep" / "report" / "kimi_ablation"),
    )
    args = parser.parse_args()

    rows = read_rows(Path(args.input))
    plot(rows, Path(args.out_prefix))
    print(f"[plot] {Path(args.out_prefix).with_suffix('.pdf')}")
    print(f"[plot] {Path(args.out_prefix).with_suffix('.png')}")


if __name__ == "__main__":
    main()
