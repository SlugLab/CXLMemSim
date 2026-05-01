#!/usr/bin/env python3
"""Plot RQ1 Graph BFS pointer-sharing vs copy-based offload across graph sizes."""
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


def parse_logs(root: pathlib.Path) -> list[dict]:
    rows: list[dict] = []
    for log in sorted(root.glob("rq1_N*.stdout.log")):
        cfg = None
        for line in log.read_text(errors="replace").splitlines():
            m = RE_CONFIG.match(line)
            if m:
                cfg = {
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


def write_csv(rows: list[dict], path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    columns = [
        "graph", "N", "device_fraction", "method", "method_name",
        "median_us", "p25_us", "p75_us", "throughput_nodes_per_s", "log",
    ]
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def plot(rows: list[dict], out_prefix: pathlib.Path, device_fraction: float) -> None:
    filtered = [
        r for r in rows
        if abs(r["device_fraction"] - device_fraction) < 1e-9
    ]
    grouped: dict[tuple[str, str], list[dict]] = defaultdict(list)
    for row in filtered:
        grouped[(row["graph"], row["method_name"])].append(row)

    fig, axes = plt.subplots(1, 2, figsize=(10, 4), sharey=True)
    labels = {"pointer-sharing": "Pointer sharing", "copy-based": "Copy-based offload"}
    colors = {"pointer-sharing": "#1f77b4", "copy-based": "#d62728"}

    for ax, graph in zip(axes, ("erdos_renyi", "barabasi_albert")):
        for method in ("pointer-sharing", "copy-based"):
            pts = sorted(grouped.get((graph, method), []), key=lambda r: r["N"])
            if not pts:
                continue
            ax.plot(
                [r["N"] for r in pts],
                [r["throughput_nodes_per_s"] for r in pts],
                marker="o",
                linewidth=2.0,
                color=colors[method],
                label=labels[method],
            )
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xticks(sorted({r["N"] for r in filtered}))
        ax.get_xaxis().set_major_formatter(plt.FuncFormatter(lambda x, _: f"{int(x / 1000)}k"))
        ax.set_title(graph.replace("_", " ").title())
        ax.set_xlabel("Vertices")
        ax.grid(True, which="both", alpha=0.25)

    axes[0].set_ylabel("BFS throughput (nodes/s), log scale")
    axes[0].legend(frameon=False)
    fig.suptitle(f"Graph BFS size scaling, avg. degree=6, device_fraction={device_fraction:.2f}")
    fig.tight_layout()

    for ext in ("pdf", "png"):
        fig.savefig(out_prefix.with_suffix(f".{ext}"), dpi=200)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="/home/victoryang00/CXLMemSim/artifact/splash_sweep/large_bfs/runs_systemd")
    parser.add_argument("--outdir", default="/home/victoryang00/CXLMemSim/artifact/splash_sweep/report")
    parser.add_argument("--device-fraction", type=float, default=1.0)
    args = parser.parse_args()

    root = pathlib.Path(args.root)
    outdir = pathlib.Path(args.outdir)
    rows = parse_logs(root)
    if not rows:
        raise SystemExit(f"No rq1_N*.stdout.log files found in {root}")

    csv_path = outdir / "bfs_size_scaling.csv"
    write_csv(rows, csv_path)
    plot(rows, outdir / "bfs_size_scaling", args.device_fraction)
    print(f"[csv] {csv_path} rows={len(rows)}")
    print(f"[plot] {outdir / 'bfs_size_scaling.pdf'}")
    print(f"[plot] {outdir / 'bfs_size_scaling.png'}")


if __name__ == "__main__":
    main()
