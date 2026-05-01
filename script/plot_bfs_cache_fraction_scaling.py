#!/usr/bin/env python3
"""Plot large Graph BFS pointer-sharing throughput across cache size and device fraction."""
from __future__ import annotations

import argparse
import csv
import pathlib
import re
from collections import defaultdict

import matplotlib.pyplot as plt

RE_CONFIG = re.compile(r"^\s*graph=(\S+)\s+N=(\d+)\s+device_fraction=([\d.]+)")
RE_METHOD = re.compile(
    r"^\s*Method\s+([AB])\s+\(([^)]+)\):\s+median=(\d+)\s+us\s+p25=(\d+)\s+us\s+p75=(\d+)\s+us"
)


def cache_bytes(label: str) -> int:
    unit = label[-1].upper()
    value = float(label[:-1] if unit in "KMG" else label)
    if unit == "K":
        return int(value * 1024)
    if unit == "M":
        return int(value * 1024 * 1024)
    if unit == "G":
        return int(value * 1024 * 1024 * 1024)
    return int(value)


def cache_label(value: int) -> str:
    if value >= 1024 * 1024 and value % (1024 * 1024) == 0:
        return f"{value // (1024 * 1024)}M"
    if value >= 1024 and value % 1024 == 0:
        return f"{value // 1024}K"
    return str(value)


def parse_logs(root: pathlib.Path) -> list[dict]:
    rows: list[dict] = []
    for log in sorted(root.glob("*/runs/rq1_N*.stdout.log")):
        cache_size = log.parents[1].name
        cfg = None
        for line in log.read_text(errors="replace").splitlines():
            m = RE_CONFIG.match(line)
            if m:
                cfg = {
                    "cache_size": cache_size,
                    "cache_bytes": cache_bytes(cache_size),
                    "graph": m.group(1),
                    "N": int(m.group(2)),
                    "device_fraction": float(m.group(3)),
                    "log": str(log),
                }
                continue
            m = RE_METHOD.match(line)
            if m and cfg:
                median_us = int(m.group(3))
                rows.append({
                    **cfg,
                    "method": m.group(1),
                    "method_name": m.group(2),
                    "median_us": median_us,
                    "p25_us": int(m.group(4)),
                    "p75_us": int(m.group(5)),
                    "throughput_nodes_per_s": cfg["N"] * 1_000_000.0 / median_us,
                })
    return rows


def read_copy_baselines(path: pathlib.Path) -> dict[tuple[int, str], float]:
    baselines: dict[tuple[int, str], float] = {}
    if not path.exists():
        return baselines
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row.get("method_name") != "copy-based":
                continue
            if float(row.get("device_fraction", "0")) != 1.0:
                continue
            baselines[(int(row["N"]), row["graph"])] = float(row["throughput_nodes_per_s"])
    return baselines


def write_csv(rows: list[dict], path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    columns = [
        "cache_size", "cache_bytes", "graph", "N", "device_fraction", "method", "method_name",
        "median_us", "p25_us", "p75_us", "throughput_nodes_per_s", "log",
    ]
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def plot(rows: list[dict], out_prefix: pathlib.Path, copy_baselines: dict[tuple[int, str], float]) -> None:
    pointer_rows = [r for r in rows if r["method_name"] == "pointer-sharing"]
    graph_sizes = sorted({r["N"] for r in pointer_rows})
    graphs = ["erdos_renyi", "barabasi_albert"]
    fractions = sorted({r["device_fraction"] for r in pointer_rows})
    grouped: dict[tuple[int, str, float], list[dict]] = defaultdict(list)
    for row in pointer_rows:
        grouped[(row["N"], row["graph"], row["device_fraction"])].append(row)

    fig, axes = plt.subplots(len(graph_sizes), len(graphs), figsize=(11, 4.2 * len(graph_sizes)),
                             squeeze=False, sharex=True, sharey=False)
    colors = plt.cm.viridis([i / max(1, len(fractions) - 1) for i in range(len(fractions))])

    for row_idx, n in enumerate(graph_sizes):
        for col_idx, graph in enumerate(graphs):
            ax = axes[row_idx][col_idx]
            for color, frac in zip(colors, fractions):
                pts = sorted(grouped.get((n, graph, frac), []), key=lambda r: r["cache_bytes"])
                if not pts:
                    continue
                ax.plot(
                    [r["cache_bytes"] / (1024 * 1024) for r in pts],
                    [r["throughput_nodes_per_s"] for r in pts],
                    marker="o",
                    linewidth=2.0,
                    color=color,
                    label=f"device_fraction={frac:.2f}",
                )
            baseline = copy_baselines.get((n, graph))
            if baseline:
                ax.axhline(baseline, color="#d62728", linestyle="--", linewidth=1.8,
                           label="copy baseline" if row_idx == 0 and col_idx == 0 else None)
            ax.set_xscale("log", base=2)
            ax.set_yscale("log")
            xticks = sorted({r["cache_bytes"] / (1024 * 1024) for r in pointer_rows})
            ax.set_xticks(xticks)
            ax.get_xaxis().set_major_formatter(
                plt.FuncFormatter(lambda x, _: cache_label(int(x * 1024 * 1024)))
            )
            ax.set_title(f"{graph.replace('_', ' ').title()}, N={n}")
            ax.set_xlabel("CXL Type-2 cache size")
            ax.grid(True, which="both", alpha=0.25)
            if col_idx == 0:
                ax.set_ylabel("BFS throughput (nodes/s)")

    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, frameon=False, ncols=min(4, len(labels)), loc="upper center")
    fig.suptitle("Large Graph BFS cache-size and device-fraction sweep", y=0.995)
    fig.tight_layout(rect=(0, 0, 1, 0.94))

    for ext in ("pdf", "png"):
        fig.savefig(out_prefix.with_suffix(f".{ext}"), dpi=200)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="/home/victoryang00/CXLMemSim/artifact/splash_sweep/cache_fraction_bfs")
    parser.add_argument("--outdir", default="/home/victoryang00/CXLMemSim/artifact/splash_sweep/report")
    parser.add_argument("--copy-baseline-csv",
                        default="/home/victoryang00/CXLMemSim/artifact/splash_sweep/report/bfs_size_scaling.csv")
    args = parser.parse_args()

    root = pathlib.Path(args.root)
    outdir = pathlib.Path(args.outdir)
    rows = parse_logs(root)
    if not rows:
        raise SystemExit(f"No rq1_N*.stdout.log files found in {root}")

    csv_path = outdir / "bfs_cache_fraction_scaling.csv"
    write_csv(rows, csv_path)
    plot(rows, outdir / "bfs_cache_fraction_scaling", read_copy_baselines(pathlib.Path(args.copy_baseline_csv)))
    print(f"[csv] {csv_path} rows={len(rows)}")
    print(f"[plot] {outdir / 'bfs_cache_fraction_scaling.pdf'}")
    print(f"[plot] {outdir / 'bfs_cache_fraction_scaling.png'}")


if __name__ == "__main__":
    main()
