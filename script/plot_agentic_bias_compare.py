#!/usr/bin/env python3
"""Parse and plot native-vs-CXL Type-2 agentic bias-mode benchmark logs."""
from __future__ import annotations

import argparse
import csv
import pathlib
import re
from collections import defaultdict

import matplotlib.pyplot as plt


RE_HEADER = re.compile(
    r"^\s*backend=(\S+)\s+steps=(\d+)\s+trials=(\d+)\s+bias_granularity=(\d+)\s+bytes"
)
RE_MODE = re.compile(r"^\[([^]]+)\]")
RE_LAT = re.compile(r"^\s*median_ns=(\d+)\s+p25_ns=(\d+)\s+p75_ns=(\d+)")
RE_RATE = re.compile(r"^\s*agent_steps_per_sec=([\d.]+)\s+checksum=(\d+)")
RE_COH = re.compile(
    r"^\s*coherency_requests=(\d+)\s+back_invalidations=(\d+)\s+writebacks=(\d+)\s+bias_flips=(\d+)"
)
RE_DIR = re.compile(
    r"^\s*snoop_hits=(\d+)\s+snoop_misses=(\d+)\s+host_bias_hits=(\d+)\s+"
    r"device_bias_hits=(\d+)\s+directory_entries=(\d+)"
)

MODES = ["host-bias", "device-bias", "hybrid-agentic", "phase-flip"]


def granularity_label(value: int) -> str:
    if value >= 1024 and value % 1024 == 0:
        return f"{value // 1024}K"
    return f"{value}B"


def parse_log(path: pathlib.Path) -> list[dict]:
    rows: list[dict] = []
    header: dict | None = None
    current: dict | None = None

    for line in path.read_text(errors="replace").splitlines():
        m = RE_HEADER.match(line)
        if m:
            header = {
                "backend": m.group(1),
                "steps": int(m.group(2)),
                "trials": int(m.group(3)),
                "bias_granularity_bytes": int(m.group(4)),
                "source_log": str(path),
            }
            continue

        m = RE_MODE.match(line)
        if m and header is not None:
            current = {**header, "mode": m.group(1)}
            rows.append(current)
            continue

        if current is None:
            continue

        m = RE_LAT.match(line)
        if m:
            current.update({
                "median_ns": int(m.group(1)),
                "p25_ns": int(m.group(2)),
                "p75_ns": int(m.group(3)),
            })
            continue

        m = RE_RATE.match(line)
        if m:
            current.update({
                "agent_steps_per_sec": float(m.group(1)),
                "checksum": int(m.group(2)),
            })
            continue

        m = RE_COH.match(line)
        if m:
            current.update({
                "coherency_requests": int(m.group(1)),
                "back_invalidations": int(m.group(2)),
                "writebacks": int(m.group(3)),
                "bias_flips": int(m.group(4)),
            })
            continue

        m = RE_DIR.match(line)
        if m:
            current.update({
                "snoop_hits": int(m.group(1)),
                "snoop_misses": int(m.group(2)),
                "host_bias_hits": int(m.group(3)),
                "device_bias_hits": int(m.group(4)),
                "directory_entries": int(m.group(5)),
            })

    return rows


def parse_logs(root: pathlib.Path) -> list[dict]:
    rows: list[dict] = []
    for log in sorted(root.glob("*agentic_bias*.log")):
        if log.name.startswith("qemu_") or log.name.startswith("cxlmemsim_"):
            continue
        rows.extend(parse_log(log))

    native_median = {
        (row["bias_granularity_bytes"], row["mode"]): row["median_ns"]
        for row in rows
        if row.get("backend") == "native" and "median_ns" in row
    }
    for row in rows:
        base = native_median.get((row["bias_granularity_bytes"], row["mode"]))
        row["slowdown_vs_native_same_gran"] = (
            row["median_ns"] / base if base and "median_ns" in row else ""
        )
        row["granularity"] = granularity_label(row["bias_granularity_bytes"])

    return rows


def write_csv(rows: list[dict], path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    columns = [
        "backend",
        "granularity",
        "bias_granularity_bytes",
        "mode",
        "steps",
        "trials",
        "median_ns",
        "p25_ns",
        "p75_ns",
        "agent_steps_per_sec",
        "slowdown_vs_native_same_gran",
        "coherency_requests",
        "back_invalidations",
        "writebacks",
        "bias_flips",
        "snoop_hits",
        "snoop_misses",
        "host_bias_hits",
        "device_bias_hits",
        "directory_entries",
        "checksum",
        "source_log",
    ]
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def plot(rows: list[dict], out_prefix: pathlib.Path) -> None:
    by_series: dict[tuple[str, int], dict[str, dict]] = defaultdict(dict)
    for row in rows:
        if "agent_steps_per_sec" not in row:
            continue
        by_series[(row["backend"], row["bias_granularity_bytes"])][row["mode"]] = row

    series = [
        ("native", 64, "Native DRAM 64B", "#4c78a8"),
        ("cxl-type2", 64, "CXL Type-2 64B", "#f58518"),
        ("native", 4096, "Native DRAM 4K", "#54a24b"),
        ("cxl-type2", 4096, "CXL Type-2 4K", "#b279a2"),
    ]
    series = [s for s in series if (s[0], s[1]) in by_series]

    fig, ax = plt.subplots(figsize=(9.4, 5.0))
    width = 0.18
    xs = list(range(len(MODES)))
    offsets = [
        (i - (len(series) - 1) / 2.0) * width
        for i in range(len(series))
    ]

    for offset, (backend, gran, label, color) in zip(offsets, series):
        values = [
            by_series.get((backend, gran), {}).get(mode, {}).get("agent_steps_per_sec", 0.0)
            for mode in MODES
        ]
        ax.bar([x + offset for x in xs], values, width=width, label=label, color=color)

    ax.set_yscale("log")
    ax.set_xticks(xs)
    ax.set_xticklabels([mode.replace("-", "\n") for mode in MODES])
    ax.set_ylabel("Agent steps/s, log scale")
    ax.set_xlabel("Bias policy")
    ax.set_title("Agentic Bias Policy: Native DRAM vs CXL Type-2 Emulator", pad=42)
    ax.grid(True, axis="y", which="both", alpha=0.25)
    ax.legend(frameon=False, ncols=4, loc="upper center", bbox_to_anchor=(0.5, 1.18))
    fig.tight_layout(rect=(0, 0, 1, 0.92))

    for ext in ("pdf", "png"):
        fig.savefig(out_prefix.with_suffix(f".{ext}"), dpi=200)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="/home/victoryang00/CXLMemSim/artifact/agentic_bias")
    parser.add_argument("--outdir", default="/home/victoryang00/CXLMemSim/artifact/agentic_bias")
    args = parser.parse_args()

    root = pathlib.Path(args.root)
    outdir = pathlib.Path(args.outdir)
    rows = parse_logs(root)
    if not rows:
        raise SystemExit(f"No agentic bias benchmark logs found in {root}")

    csv_path = outdir / "agentic_bias_compare.csv"
    write_csv(rows, csv_path)
    plot(rows, outdir / "agentic_bias_compare")
    print(f"[csv] {csv_path} rows={len(rows)}")
    print(f"[plot] {outdir / 'agentic_bias_compare.pdf'}")
    print(f"[plot] {outdir / 'agentic_bias_compare.png'}")


if __name__ == "__main__":
    main()
