#!/usr/bin/env python3
"""Create the audited real-CXL.mem host-coherence paper figure."""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


VALIDATION_SCHEMA = "splash.cxlmem-hwcc-validation.v1"
RESULT_FIELDS = ("backend", "mode", "placement", "repetition", "event", "value", "operations")
OUTPUT_FIELDS = (
    "panel",
    "series",
    "backend",
    "mode",
    "placement",
    "event",
    "unit",
    "n",
    "median",
    "p25",
    "p75",
    "stale_reads",
    "atomic_errors",
)
LATENCY_EVENT = "benchmark.average_ns"
CPU_CXL_EVENT = "mem_load_retired.local_cxl_mem"
XSNP_EVENT = "mem_load_l3_hit_retired.xsnp_fwd"
DEVICE_READ_EVENT = "cxl_pmu_mem0.0/m2s_req_memrd/"

SERIES = (
    ("latency", "DRAM warm load", "dram", "warm-load", "same-numa", LATENCY_EVENT, "ns/op"),
    ("latency", "DRAM cold load", "dram", "cold-load", "same-numa", LATENCY_EVENT, "ns/op"),
    ("latency", "CXL warm load", "cxlmem", "warm-load", "same-numa", LATENCY_EVENT, "ns/op"),
    ("latency", "CXL cold load", "cxlmem", "cold-load", "same-numa", LATENCY_EVENT, "ns/op"),
    ("latency", "CXL handoff same NUMA", "cxlmem", "handoff", "same-numa", LATENCY_EVENT, "ns/op"),
    ("latency", "CXL handoff cross NUMA", "cxlmem", "handoff", "cross-numa", LATENCY_EVENT, "ns/op"),
    ("pmu", "CXL cold CPU CXL-read", "cxlmem", "cold-load", "same-numa", CPU_CXL_EVENT, "events/op"),
    ("pmu", "CXL cold device MemRd", "cxlmem", "cold-load", "same-numa", DEVICE_READ_EVENT, "events/op"),
    ("pmu", "CXL handoff same NUMA XSNP", "cxlmem", "handoff", "same-numa", XSNP_EVENT, "events/op"),
    ("pmu", "CXL handoff cross NUMA XSNP", "cxlmem", "handoff", "cross-numa", XSNP_EVENT, "events/op"),
)


class PlotValidationError(ValueError):
    """Raised when an audit directory cannot support the figure."""


@dataclass(frozen=True)
class GeneratedOutputs:
    csv_path: Path
    pdf_path: Path


def _read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PlotValidationError(f"{path.name} is missing or malformed") from error


def _quantile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def _format(value: float) -> str:
    return f"{value:.9g}"


def _load_run(run_dir: Path) -> tuple[list[dict[str, str]], int, int]:
    validation = _read_json(run_dir / "validation.json")
    if not isinstance(validation, dict) or validation.get("schema") != VALIDATION_SCHEMA or validation.get("status") != "pass":
        raise PlotValidationError("a validated passing run is required")

    summary = _read_json(run_dir / "summary.json")
    try:
        stale_reads = int(summary["litmus"]["stale"])
        atomic_errors = int(summary["atomic"]["ticket_errors"]) + int(summary["atomic"]["cas_errors"])
    except (KeyError, TypeError, ValueError) as error:
        raise PlotValidationError("summary correctness counters are missing or malformed") from error
    if stale_reads != 0 or atomic_errors != 0:
        raise PlotValidationError("passing figure input must have zero correctness errors")

    try:
        with (run_dir / "results.csv").open(newline="", encoding="utf-8") as source:
            reader = csv.DictReader(source)
            rows = list(reader)
    except (OSError, csv.Error) as error:
        raise PlotValidationError("results.csv is missing or malformed") from error
    if tuple(reader.fieldnames or ()) != RESULT_FIELDS or not rows:
        raise PlotValidationError("results.csv does not follow the hardware-validation contract")
    for row in rows:
        try:
            value = float(row["value"])
            operations = int(row["operations"])
            int(row["repetition"])
        except (TypeError, ValueError) as error:
            raise PlotValidationError("results.csv contains malformed numeric values") from error
        if not math.isfinite(value) or operations <= 0:
            raise PlotValidationError("results.csv contains invalid values or operation counts")
    return rows, stale_reads, atomic_errors


def _aggregate(rows: list[dict[str, str]], stale_reads: int, atomic_errors: int) -> list[dict[str, str]]:
    grouped: dict[tuple[str, str, str, str], list[float]] = defaultdict(list)
    for row in rows:
        key = (row["backend"], row["mode"], row["placement"], row["event"])
        value = float(row["value"])
        if row["event"] != LATENCY_EVENT:
            value /= int(row["operations"])
        grouped[key].append(value)

    records: list[dict[str, str]] = []
    for panel, label, backend, mode, placement, event, unit in SERIES:
        values = grouped.get((backend, mode, placement, event))
        if not values:
            continue
        records.append(
            {
                "panel": panel,
                "series": label,
                "backend": backend,
                "mode": mode,
                "placement": placement,
                "event": event,
                "unit": unit,
                "n": str(len(values)),
                "median": _format(_quantile(values, 0.5)),
                "p25": _format(_quantile(values, 0.25)),
                "p75": _format(_quantile(values, 0.75)),
                "stale_reads": str(stale_reads),
                "atomic_errors": str(atomic_errors),
            }
        )
    if not records:
        raise PlotValidationError("results.csv contains none of the required figure series")
    return records


def _plot(records: list[dict[str, str]], output: Path) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np

    matplotlib.rcParams.update(
        {
            "font.family": "DejaVu Sans",
            "font.size": 8,
            "axes.labelsize": 8,
            "axes.titlesize": 9,
            "xtick.labelsize": 7,
            "ytick.labelsize": 7,
            "legend.fontsize": 7,
            "pdf.compression": 9,
        }
    )
    colors = ("#0072B2", "#E69F00", "#009E73", "#D55E00", "#56B4E9", "#CC79A7")
    fig, axes = plt.subplots(1, 2, figsize=(7.15, 2.65), constrained_layout=True)
    for axis, panel, title, ylabel in (
        (axes[0], "latency", "(a) Host access and ownership latency", "Median latency (ns/op)"),
        (axes[1], "pmu", "(b) Coherence and CXL.mem PMU evidence", "Median events / operation"),
    ):
        subset = [record for record in records if record["panel"] == panel]
        x = np.arange(len(subset))
        medians = np.array([float(record["median"]) for record in subset])
        lower = medians - np.array([float(record["p25"]) for record in subset])
        upper = np.array([float(record["p75"]) for record in subset]) - medians
        axis.bar(x, medians, color=colors[: len(subset)], width=0.72, edgecolor="#222222", linewidth=0.4)
        axis.errorbar(x, medians, yerr=np.vstack((lower, upper)), fmt="none", ecolor="#222222", capsize=2, linewidth=0.8)
        axis.set_xticks(x, [record["series"].replace("CXL ", "").replace(" NUMA", "") for record in subset], rotation=28, ha="right")
        axis.set_ylabel(ylabel)
        axis.set_title(title)
        axis.grid(axis="y", color="#d0d0d0", linewidth=0.5, alpha=0.8)
        axis.set_axisbelow(True)
        axis.spines[["top", "right"]].set_visible(False)
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, metadata={"Creator": "CXLMemSim", "CreationDate": None, "ModDate": None})
    plt.close(fig)


def generate_outputs(run_dir: Path, output_dir: Path) -> GeneratedOutputs:
    rows, stale_reads, atomic_errors = _load_run(run_dir)
    records = _aggregate(rows, stale_reads, atomic_errors)
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "host_coherence.csv"
    pdf_path = output_dir / "host_coherence.pdf"
    with csv_path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=OUTPUT_FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(records)
    _plot(records, pdf_path)
    return GeneratedOutputs(csv_path=csv_path, pdf_path=pdf_path)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--output-dir", type=Path)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    output_dir = args.output_dir or args.run_dir / "paper"
    try:
        outputs = generate_outputs(args.run_dir, output_dir)
    except PlotValidationError as error:
        print(f"error: {error}")
        return 1
    print(outputs.csv_path)
    print(outputs.pdf_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
