#!/usr/bin/env python3
"""Plot CXL directory/cache footprint and BFS size scaling from splash sweep CSVs."""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def to_float(row: dict[str, str], key: str) -> float | None:
    try:
        return float(row[key])
    except (KeyError, TypeError, ValueError):
        return None


def to_int(row: dict[str, str], key: str) -> int | None:
    value = to_float(row, key)
    return None if value is None else int(value)


def label_for(graph: str, method_name: str, device_fraction: float) -> str:
    graph_label = "ER" if graph == "erdos_renyi" else "BA" if graph == "barabasi_albert" else graph
    method_label = "ptr-share" if method_name == "pointer-sharing" else "copy"
    return f"{graph_label} {method_label}, dev={device_fraction:g}"


def plot(csv_dir: Path, outdir: Path) -> tuple[Path, Path]:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    bfs_rows = read_csv(csv_dir / "rq1_bfs.csv")
    hash_rows = read_csv(csv_dir / "rq1_hash.csv")
    if not bfs_rows:
        raise SystemExit(f"No BFS rows found in {csv_dir / 'rq1_bfs.csv'}")

    groups: dict[tuple[str, str, float], list[tuple[int, float, float]]] = defaultdict(list)
    for row in bfs_rows:
        n = to_int(row, "N")
        median_us = to_float(row, "median_us")
        dir_entries = to_float(row, "dir_entries")
        device_fraction = to_float(row, "device_fraction")
        if n is None or median_us is None or dir_entries is None or device_fraction is None:
            continue
        if device_fraction not in (0.0, 1.0):
            continue
        groups[(row.get("graph", ""), row.get("method_name", row.get("method", "")), device_fraction)].append(
            (n, median_us, dir_entries)
        )

    outdir.mkdir(parents=True, exist_ok=True)
    fig, (ax_cache, ax_bfs) = plt.subplots(1, 2, figsize=(11.5, 4.2), constrained_layout=True)

    palette = {
        ("pointer-sharing", 0.0): "#2563eb",
        ("pointer-sharing", 1.0): "#0891b2",
        ("copy-based", 0.0): "#dc2626",
        ("copy-based", 1.0): "#f97316",
    }
    markers = {"erdos_renyi": "o", "barabasi_albert": "s"}

    for (graph, method_name, device_fraction), points in sorted(groups.items()):
        points.sort()
        xs = [p[0] for p in points]
        lat = [p[1] for p in points]
        entries = [p[2] for p in points]
        color = palette.get((method_name, device_fraction), "#374151")
        marker = markers.get(graph, "o")
        label = label_for(graph, method_name, device_fraction)
        ax_cache.plot(xs, entries, marker=marker, linewidth=1.8, markersize=4.5, color=color, label=label)
        ax_bfs.plot(xs, lat, marker=marker, linewidth=1.8, markersize=4.5, color=color, label=label)

    ax_cache.set_xscale("log")
    ax_cache.set_yscale("log")
    ax_cache.set_xlabel("BFS graph size N")
    ax_cache.set_ylabel("CXL directory entries")
    ax_cache.set_title("CXL Cache Footprint")
    ax_cache.grid(True, which="both", alpha=0.25)

    ax_bfs.set_xscale("log")
    ax_bfs.set_yscale("log")
    ax_bfs.set_xlabel("BFS graph size N")
    ax_bfs.set_ylabel("Median BFS latency (us)")
    ax_bfs.set_title("BFS Size Scaling")
    ax_bfs.grid(True, which="both", alpha=0.25)
    ax_bfs.legend(fontsize=7, ncol=1, loc="best")

    if not hash_rows:
        fig.text(
            0.01,
            0.01,
            "Note: rq1_hash.csv contains no data rows; plotted rq1_bfs.csv.",
            fontsize=8,
            color="#555555",
        )

    pdf = outdir / "cxl_cache_bfs_scaling.pdf"
    png = outdir / "cxl_cache_bfs_scaling.png"
    fig.savefig(pdf)
    fig.savefig(png, dpi=200)
    plt.close(fig)
    return pdf, png


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv-dir", default="artifact/splash_sweep/csv")
    ap.add_argument("--outdir", default="artifact/splash_sweep/report")
    args = ap.parse_args()

    pdf, png = plot(Path(args.csv_dir), Path(args.outdir))
    print(f"Wrote {pdf}")
    print(f"Wrote {png}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
