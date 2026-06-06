#!/usr/bin/env python3
import argparse
import csv
import os
import textwrap
from pathlib import Path
from statistics import mean, median

os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")

import h5py
import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np


ROOT = Path(__file__).resolve().parents[1]

TOKENS = {
    "surface": "#FCFCFD",
    "panel": "#FFFFFF",
    "ink": "#1F2430",
    "muted": "#6F768A",
    "grid": "#E6E8F0",
    "axis": "#D7DBE7",
}

COLORS = {
    "faiss_native": "#A3BEFA",
    "faiss_native_edge": "#2E4780",
    "faiss_cxl": "#F0986E",
    "faiss_cxl_edge": "#804126",
    "vsag": "#A3D576",
    "vsag_edge": "#386411",
    "qdrant": "#FFE15B",
    "qdrant_edge": "#736422",
}


def find_latest_faiss_csv():
    candidates = sorted(
        (ROOT / "artifact/faiss_splash").glob("run_*/results.csv"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    if not candidates:
        raise FileNotFoundError("no FAISS results.csv found under artifact/faiss_splash/run_*")
    return candidates[0]


def read_faiss(csv_path):
    rows = []
    with csv_path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            rows.append(
                {
                    "node": int(row["node"]),
                    "storage": row["storage"],
                    "qps": float(row["qps"]),
                    "total_ms": float(row["total_ms"]),
                    "pool_read_ms": float(row["pool_read_ms"]),
                    "nb": int(row["nb"]),
                    "nq": int(row["nq"]),
                    "dim": int(row["dim"]),
                    "k": int(row["k"]),
                }
            )
    if not rows:
        raise ValueError(f"{csv_path} has no result rows")
    return rows


def recent_hdf5_files(algo):
    algo_dir = ROOT / "workloads/ann-benchmarks/results/random-xs-20-angular/10" / algo
    files = sorted(algo_dir.glob("*.hdf5"), key=lambda path: path.stat().st_mtime, reverse=True)
    if not files:
        raise FileNotFoundError(f"no {algo} HDF5 files found under {algo_dir}")
    newest = files[0].stat().st_mtime
    recent = [path for path in files if newest - path.stat().st_mtime <= 10 * 60]
    return recent or files


def read_ann_best(algo):
    records = []
    for path in recent_hdf5_files(algo):
        with h5py.File(path, "r") as handle:
            times = np.asarray(handle["times"][:], dtype=float)
            avg_seconds = float(np.mean(times))
            qps = 1.0 / avg_seconds if avg_seconds > 0 else 0.0
            records.append(
                {
                    "algorithm": algo,
                    "qps": qps,
                    "avg_query_ms": avg_seconds * 1000.0,
                    "build_time_s": float(handle.attrs.get("build_time", 0.0)),
                    "index_size_kb": float(handle.attrs.get("index_size", 0.0)),
                    "name": str(handle.attrs.get("name", path.stem)),
                    "path": path,
                }
            )
    return max(records, key=lambda row: row["qps"]), records


def fmt_qps(value):
    if value >= 1000000:
        return f"{value / 1000000:.1f}M/s"
    if value >= 1000:
        return f"{value / 1000:.1f}k/s"
    return f"{value:.1f}/s"


def fmt_axis(value, _pos=None):
    if value >= 1000:
        return f"{value / 1000:g}k"
    return f"{value:g}"


def style_axis(ax):
    ax.set_facecolor(TOKENS["panel"])
    ax.grid(True, axis="x", color=TOKENS["grid"], linewidth=0.8)
    ax.grid(False, axis="y")
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(TOKENS["axis"])
    ax.tick_params(colors=TOKENS["muted"], labelsize=9)
    ax.xaxis.label.set_color(TOKENS["ink"])
    ax.yaxis.label.set_color(TOKENS["ink"])


def add_header(fig, title, subtitle):
    fig.text(0.065, 0.965, title, ha="left", va="top", fontsize=16, fontweight="semibold", color=TOKENS["ink"])
    fig.text(0.065, 0.925, subtitle, ha="left", va="top", fontsize=10, color=TOKENS["muted"])


def build_summary(faiss_rows, vsag_best, qdrant_best):
    native_qps = [row["qps"] for row in faiss_rows if row["storage"] == "native"]
    cxl_qps = [row["qps"] for row in faiss_rows if row["storage"] == "cxl-pool"]
    if not native_qps or not cxl_qps:
        raise ValueError("FAISS rows must include native and cxl-pool storage")
    return [
        {
            "label": "FAISS native",
            "algorithm": "faiss",
            "qps": median(native_qps),
            "detail": f"median across {len(native_qps)} nodes",
            "color": COLORS["faiss_native"],
            "edge": COLORS["faiss_native_edge"],
        },
        {
            "label": "FAISS CXL-pool",
            "algorithm": "faiss",
            "qps": median(cxl_qps),
            "detail": f"median across {len(cxl_qps)} nodes",
            "color": COLORS["faiss_cxl"],
            "edge": COLORS["faiss_cxl_edge"],
        },
        {
            "label": "VSAG best",
            "algorithm": "vsag",
            "qps": vsag_best["qps"],
            "detail": vsag_best["name"],
            "color": COLORS["vsag"],
            "edge": COLORS["vsag_edge"],
        },
        {
            "label": "Qdrant best",
            "algorithm": "qdrant",
            "qps": qdrant_best["qps"],
            "detail": qdrant_best["name"],
            "color": COLORS["qdrant"],
            "edge": COLORS["qdrant_edge"],
        },
    ]


def write_summary(out_dir, summary, vsag_best, qdrant_best, faiss_csv):
    out_path = out_dir / "vector_workload_qps_summary.csv"
    with out_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=["label", "algorithm", "qps", "detail", "source"])
        writer.writeheader()
        for row in summary:
            source = faiss_csv
            if row["algorithm"] == "vsag":
                source = vsag_best["path"]
            elif row["algorithm"] == "qdrant":
                source = qdrant_best["path"]
            writer.writerow(
                {
                    "label": row["label"],
                    "algorithm": row["algorithm"],
                    "qps": f"{row['qps']:.6f}",
                    "detail": row["detail"],
                    "source": source,
                }
            )
    return out_path


def draw_chart(out_dir, summary, faiss_rows, faiss_csv):
    plt.rcParams.update(
        {
            "figure.facecolor": TOKENS["surface"],
            "savefig.facecolor": TOKENS["surface"],
            "axes.edgecolor": TOKENS["axis"],
            "axes.labelcolor": TOKENS["ink"],
            "font.family": "DejaVu Sans",
        }
    )

    fig = plt.figure(figsize=(13.2, 7.4), facecolor=TOKENS["surface"])
    grid = fig.add_gridspec(2, 1, height_ratios=[1.15, 1.0], hspace=0.55)
    ax_top = fig.add_subplot(grid[0, 0])
    ax_bottom = fig.add_subplot(grid[1, 0])

    plot_rows = sorted(summary, key=lambda row: row["qps"])
    labels = [row["label"] for row in plot_rows]
    qps = [row["qps"] for row in plot_rows]
    y = np.arange(len(plot_rows))
    bars = ax_top.barh(
        y,
        qps,
        color=[row["color"] for row in plot_rows],
        edgecolor=[row["edge"] for row in plot_rows],
        linewidth=1.0,
    )
    ax_top.set_yticks(y, labels)
    ax_top.set_xscale("log")
    ax_top.set_xlim(max(5, min(qps) * 0.55), max(qps) * 2.2)
    ax_top.xaxis.set_major_formatter(mticker.FuncFormatter(fmt_axis))
    ax_top.set_xlabel("Queries per second, log scale")
    ax_top.set_ylabel("")
    style_axis(ax_top)
    for bar, row in zip(bars, plot_rows):
        ax_top.text(
            bar.get_width() * 1.08,
            bar.get_y() + bar.get_height() / 2,
            fmt_qps(row["qps"]),
            va="center",
            ha="left",
            fontsize=9,
            color=TOKENS["ink"],
        )

    nodes = sorted({row["node"] for row in faiss_rows})
    native = {row["node"]: row["qps"] for row in faiss_rows if row["storage"] == "native"}
    cxl = {row["node"]: row["qps"] for row in faiss_rows if row["storage"] == "cxl-pool"}
    ax_bottom.plot(
        nodes,
        [native[node] for node in nodes],
        marker="o",
        color=COLORS["faiss_native_edge"],
        markerfacecolor=COLORS["faiss_native"],
        markeredgecolor=COLORS["faiss_native_edge"],
        linewidth=1.2,
        label=f"Native median {fmt_qps(median(native.values()))}",
    )
    ax_bottom.plot(
        nodes,
        [cxl[node] for node in nodes],
        marker="o",
        color=COLORS["faiss_cxl_edge"],
        markerfacecolor=COLORS["faiss_cxl"],
        markeredgecolor=COLORS["faiss_cxl_edge"],
        linewidth=1.2,
        label=f"CXL-pool median {fmt_qps(median(cxl.values()))}",
    )
    ax_bottom.set_yscale("log")
    ax_bottom.yaxis.set_major_formatter(mticker.FuncFormatter(fmt_axis))
    ax_bottom.set_xticks(nodes)
    ax_bottom.set_xlabel("FAISS node")
    ax_bottom.set_ylabel("Queries per second, log scale")
    ax_bottom.legend(loc="lower left", bbox_to_anchor=(0, 1.02), frameon=False, ncol=2, borderaxespad=0)
    style_axis(ax_bottom)

    first = faiss_rows[0]
    add_header(
        fig,
        "Vector workload throughput from the latest smoke run",
        "FAISS uses synthetic vectors from the Splash SHMEM run; VSAG and Qdrant use ANN-Benchmarks random-xs-20-angular. Values are observed QPS from local runs.",
    )
    footer = (
        f"Sources: {faiss_csv.relative_to(ROOT)}; ANN-Benchmarks HDF5 result files. "
        f"FAISS setup: nb={first['nb']}, nq={first['nq']}, dim={first['dim']}, k={first['k']}. "
        "Use this as a smoke-run graph, not a normalized benchmark across identical datasets."
    )
    fig.text(
        0.065,
        0.032,
        textwrap.fill(footer, width=185),
        ha="left",
        va="bottom",
        fontsize=8.5,
        color=TOKENS["muted"],
    )
    fig.subplots_adjust(left=0.18, right=0.96, top=0.86, bottom=0.13)

    png = out_dir / "vector_workload_qps.png"
    svg = out_dir / "vector_workload_qps.svg"
    fig.savefig(png, dpi=180)
    fig.savefig(svg)
    plt.close(fig)
    return png, svg


def main():
    parser = argparse.ArgumentParser(description="Draw vector workload benchmark graphs from local run artifacts.")
    parser.add_argument("--faiss-csv", type=Path, default=None)
    parser.add_argument("--out-dir", type=Path, default=ROOT / "artifact/vector_workload_graph")
    args = parser.parse_args()

    faiss_csv = args.faiss_csv.resolve() if args.faiss_csv else find_latest_faiss_csv()
    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    faiss_rows = read_faiss(faiss_csv)
    vsag_best, _vsag_records = read_ann_best("vsag")
    qdrant_best, _qdrant_records = read_ann_best("qdrant")
    summary = build_summary(faiss_rows, vsag_best, qdrant_best)
    summary_csv = write_summary(out_dir, summary, vsag_best, qdrant_best, faiss_csv)
    png, svg = draw_chart(out_dir, summary, faiss_rows, faiss_csv)

    print(f"wrote {png}")
    print(f"wrote {svg}")
    print(f"wrote {summary_csv}")


if __name__ == "__main__":
    main()
