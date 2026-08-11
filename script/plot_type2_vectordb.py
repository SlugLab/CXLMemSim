#!/usr/bin/env python3
"""Validate and plot the Type-2 VectorDB paper subset."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Sequence

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from script.run_type2_vectordb import MODES, SUMMARY_FIELDNAMES, validate_run_dir


PAPER_WORKLOADS = frozenset(
    (
        rows,
        128,
        queries,
        10,
        ratio,
        5,
        10,
        1,
    )
    for rows, queries, ratio in (
        (16384, 16, 0.001),
        (65536, 1, 0.001),
        (65536, 16, 0.0),
        (65536, 16, 0.0001),
        (65536, 16, 0.001),
        (65536, 16, 0.01),
        (65536, 64, 0.001),
        (262144, 16, 0.001),
    )
)
PERFORMANCE_MODES = ("type2-hwcc", "software-cc", "full-copy", "native-gpu")
UPDATE_MODES = ("type2-hwcc", "software-cc", "full-copy")
OUTPUT_STEM = "type2_vectordb_paper_subset"
PERFORMANCE_STEM = "type2_vectordb_performance"
UPDATE_COST_STEM = "type2_vectordb_update_cost"
PROVENANCE_FIELDS = (
    "source_run_id",
    "source_run_dir",
    "schema",
    "started_utc",
    "completed_utc",
    "superproject_commit",
    "qemu_commit",
    "gpu_uuid",
    "gpu_name",
    "experiment_command",
    "base_image",
    "kernel_image",
    "qemu_binary",
    "real_libcuda",
    "server_binary",
    "plotter_commit",
    "plotter_sha256",
    "manifest_sha256",
    "results_sha256",
    "summary_sha256",
    "panel_membership",
)
MODE_LABELS = {
    "type2-hwcc": "Type-2 HWCC",
    "software-cc": "Software CC",
    "full-copy": "Full copy",
    "native-gpu": "Native GPU (compute-only)",
}
MODE_LINESTYLES = {"type2-hwcc": "-", "software-cc": "-", "full-copy": "-", "native-gpu": "--"}
MODE_STYLES = {
    "type2-hwcc": ("#0072B2", "o"),
    "software-cc": ("#D55E00", "s"),
    "full-copy": ("#CC79A7", "^"),
    "native-gpu": ("#009E73", "D"),
}


class PlotValidationError(ValueError):
    """Raised when an input run cannot support the paper figure."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _plotter_commit() -> str:
    repository = Path(__file__).resolve().parents[1]
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=repository,
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise PlotValidationError("plotter git revision is unavailable") from error
    commit = result.stdout.strip()
    if len(commit) != 40:
        raise PlotValidationError("plotter git revision is malformed")
    return commit


def _experiment_command(manifest: dict[str, Any]) -> str:
    commands = manifest.get("commands")
    if not isinstance(commands, list) or not all(isinstance(command, str) for command in commands):
        raise PlotValidationError("manifest commands are missing or malformed")
    matches = [command for command in commands if "vectordb_shared_index --mode" in command]
    if not matches:
        raise PlotValidationError("manifest does not record a VectorDB benchmark command")
    return matches[0]


def _read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PlotValidationError(f"{path.name} is missing or malformed") from error


def _canonical_workload(workload: Any) -> tuple[int, int, int, int, float, int, int, int] | None:
    if not isinstance(workload, dict):
        return None
    try:
        values = (
            int(workload["rows"]),
            int(workload["dim"]),
            int(workload["queries"]),
            int(workload["topk"]),
            float(workload["update_ratio"]),
            int(workload["warmup"]),
            int(workload["epochs"]),
            int(workload["seed"]),
        )
    except (KeyError, TypeError, ValueError, OverflowError):
        return None
    if not math.isfinite(values[4]):
        return None
    return values


def _load_results(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise PlotValidationError("results.jsonl is missing") from error
    for number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as error:
            raise PlotValidationError(f"results.jsonl:{number}: malformed JSON") from error
        if not isinstance(row, dict):
            raise PlotValidationError(f"results.jsonl:{number}: row must be an object")
        rows.append(row)
    return rows


def _load_summary(path: Path) -> list[dict[str, str]]:
    try:
        with path.open(newline="", encoding="utf-8") as source:
            reader = csv.DictReader(source)
            rows = list(reader)
    except (OSError, csv.Error) as error:
        raise PlotValidationError("summary.csv is missing or malformed") from error
    if tuple(reader.fieldnames or ()) != SUMMARY_FIELDNAMES or not rows:
        raise PlotValidationError("summary.csv does not follow the validated summary contract")
    return rows


def _validate_and_load(run_dir: Path) -> tuple[dict[str, Any], list[dict[str, Any]], list[dict[str, str]]]:
    manifest_path = run_dir / "manifest.json"
    results_path = run_dir / "results.jsonl"
    summary_path = run_dir / "summary.csv"
    manifest = _read_json(manifest_path)
    if not isinstance(manifest, dict):
        raise PlotValidationError("manifest.json must contain an object")
    if manifest.get("schema") != "splash.vectordb.v1" or manifest.get("status") != "pass":
        raise PlotValidationError("manifest must be a completed splash.vectordb.v1 run")
    for field in ("started_utc", "completed_utc"):
        if not isinstance(manifest.get(field), str) or not manifest[field]:
            raise PlotValidationError(f"manifest {field} is required")
    inputs = manifest.get("inputs")
    required_inputs = ("base_image", "kernel_image", "qemu_binary", "real_libcuda", "server_binary")
    if not isinstance(inputs, dict) or any(not isinstance(inputs.get(field), str) or not inputs[field] for field in required_inputs):
        raise PlotValidationError("manifest inputs must record all experiment binaries and images")
    _experiment_command(manifest)

    workloads = manifest.get("workloads")
    canonical = [_canonical_workload(workload) for workload in workloads] if isinstance(workloads, list) else []
    if len(canonical) != 8 or None in canonical or len(set(canonical)) != 8 or set(canonical) != PAPER_WORKLOADS:
        raise PlotValidationError("manifest must contain exactly the 8 paper-subset workloads")

    errors = validate_run_dir(run_dir)
    if errors:
        raise PlotValidationError("validated run required: " + "; ".join(errors))

    results = _load_results(results_path)
    summary = _load_summary(summary_path)
    expected_result_count = len(PAPER_WORKLOADS) * len(MODES) * 10
    if len(results) != expected_result_count:
        raise PlotValidationError(f"paper subset requires {expected_result_count} validated result rows")
    if {row.get("mode") for row in results} != set(MODES):
        raise PlotValidationError("paper subset requires exactly the five experiment modes")
    if {row.get("mode") for row in results if row.get("stale_observed") is True} != {"negative-stale"}:
        raise PlotValidationError("only negative-stale may report stale observations")

    first = results[0]
    identity_fields = ("schema", "superproject_commit", "qemu_commit", "gpu_uuid", "gpu_name")
    for field in identity_fields:
        values = {row.get(field) for row in results}
        if len(values) != 1 or first.get(field) in (None, ""):
            raise PlotValidationError(f"results must have one consistent {field}")
    commits = manifest.get("commits")
    if not isinstance(commits, dict) or commits.get("superproject") != first["superproject_commit"]:
        raise PlotValidationError("manifest superproject commit does not match all results")
    if commits.get("qemu") != first["qemu_commit"] or commits.get("qemu_gitlink") != first["qemu_commit"]:
        raise PlotValidationError("manifest QEMU commits do not match all results")

    summary_modes = {row.get("mode") for row in summary}
    if summary_modes != set(MODES) or len(summary) != len(PAPER_WORKLOADS) * len(MODES):
        raise PlotValidationError("summary.csv must contain exactly five modes for all 8 workloads")
    return manifest, results, summary


def _panel_membership(record: dict[str, str]) -> str:
    rows = int(record["rows"])
    queries = int(record["queries"])
    ratio = float(record["update_ratio"])
    panels: list[str] = []
    if queries == 16 and math.isclose(ratio, 0.001, abs_tol=1e-12):
        panels.append("index-size")
    if rows == 65536 and math.isclose(ratio, 0.001, abs_tol=1e-12):
        panels.append("query-batch")
    if rows == 65536 and queries == 16:
        panels.append("update-ratio")
    return ";".join(panels)


def _build_aggregate(
    run_dir: Path,
    manifest: dict[str, Any],
    results: list[dict[str, Any]],
    summary: list[dict[str, str]],
) -> list[dict[str, str]]:
    first = results[0]
    inputs = manifest["inputs"]
    provenance = {
        "source_run_id": run_dir.name,
        "source_run_dir": str(run_dir.resolve()),
        "schema": str(first["schema"]),
        "started_utc": manifest["started_utc"],
        "completed_utc": manifest["completed_utc"],
        "superproject_commit": str(first["superproject_commit"]),
        "qemu_commit": str(first["qemu_commit"]),
        "gpu_uuid": str(first["gpu_uuid"]),
        "gpu_name": str(first["gpu_name"]),
        "experiment_command": _experiment_command(manifest),
        "base_image": inputs["base_image"],
        "kernel_image": inputs["kernel_image"],
        "qemu_binary": inputs["qemu_binary"],
        "real_libcuda": inputs["real_libcuda"],
        "server_binary": inputs["server_binary"],
        "plotter_commit": _plotter_commit(),
        "plotter_sha256": _sha256(Path(__file__)),
        "manifest_sha256": _sha256(run_dir / "manifest.json"),
        "results_sha256": _sha256(run_dir / "results.jsonl"),
        "summary_sha256": _sha256(run_dir / "summary.csv"),
    }
    aggregate: list[dict[str, str]] = []
    for record in summary:
        if record["mode"] == "negative-stale":
            continue
        output = dict(provenance)
        output["panel_membership"] = _panel_membership(record)
        output.update(record)
        aggregate.append(output)
    aggregate.sort(
        key=lambda row: (
            int(row["rows"]),
            int(row["queries"]),
            float(row["update_ratio"]),
            PERFORMANCE_MODES.index(row["mode"]),
        )
    )
    return aggregate


def _metric_lookup(records: list[dict[str, str]]) -> dict[tuple[str, int, int, float], dict[str, str]]:
    lookup: dict[tuple[str, int, int, float], dict[str, str]] = {}
    for record in records:
        key = (record["mode"], int(record["rows"]), int(record["queries"]), float(record["update_ratio"]))
        if key in lookup:
            raise PlotValidationError("aggregated CSV contains a duplicate performance point")
        lookup[key] = record
    return lookup


def _use_log_scale(values: Sequence[float]) -> bool:
    """Use logarithmic presentation only when it materially improves readability."""
    return bool(values) and min(values) > 0 and max(values) / min(values) >= 20.0


def _errorbar_values(
    lookup: dict[tuple[str, int, int, float], dict[str, str]],
    mode: str,
    x_values: Sequence[int | float],
    key_builder: Any,
    metric: str,
    scale: float = 1.0,
) -> tuple[list[float], list[float], list[float]]:
    medians: list[float] = []
    lower: list[float] = []
    upper: list[float] = []
    for x_value in x_values:
        record = lookup[key_builder(mode, x_value)]
        median = float(record[f"{metric}_median"]) / scale
        p25 = float(record[f"{metric}_p25"]) / scale
        p75 = float(record[f"{metric}_p75"]) / scale
        if not all(math.isfinite(value) and value >= 0 for value in (median, p25, p75)) or not p25 <= median <= p75:
            raise PlotValidationError(f"{metric} must contain ordered finite nonnegative quartiles")
        medians.append(median)
        lower.append(median - p25)
        upper.append(p75 - median)
    return medians, lower, upper


def _figure_metadata(provenance: dict[str, str], title: str) -> tuple[dict[str, str], dict[str, str]]:
    description = (
        "Validated Type-2 VectorDB paper subset. Negative-stale is verified but excluded from performance curves. "
        f"manifest sha256={provenance['manifest_sha256']}"
    )
    pdf = {
        "Title": title,
        "Author": "CXLMemSim",
        "Subject": description,
        "Keywords": "CXL Type-2, VectorDB, coherence",
        "Creator": "script/plot_type2_vectordb.py",
    }
    png = {"Software": "script/plot_type2_vectordb.py", "Description": description}
    return pdf, png


def _add_footer(figure: Any, provenance: dict[str, str]) -> None:
    footer = (
        f"Run {provenance['source_run_id']} | experiment {provenance['superproject_commit'][:12]} | "
        f"QEMU {provenance['qemu_commit'][:12]} | plotter {provenance['plotter_commit'][:12]} | "
        f"{provenance['gpu_name']}"
    )
    figure.text(0.5, 0.008, footer, ha="center", va="bottom", fontsize=6.2, color="#444444")


def _plot_performance(
    records: list[dict[str, str]], provenance: dict[str, str], pdf_path: Path, png_path: Path
) -> None:
    os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    lookup = _metric_lookup(records)
    figure, axes = plt.subplots(1, 3, figsize=(11.2, 3.45))
    panel_specs = (
        {
            "title": "(a) Index-size scaling",
            "x_values": (16384, 65536, 262144),
            "x_labels": ("8", "32", "128"),
            "x_label": "Index size (MiB)",
            "metric": "qps",
            "y_label": "Median QPS",
            "key": lambda mode, value: (mode, value, 16, 0.001),
            "allow_log_y": True,
        },
        {
            "title": "(b) Query-batch scaling",
            "x_values": (1, 16, 64),
            "x_labels": ("1", "16", "64"),
            "x_label": "Queries per batch",
            "metric": "qps",
            "y_label": "Median QPS",
            "key": lambda mode, value: (mode, 65536, value, 0.001),
            "allow_log_y": True,
        },
        {
            "title": "(c) Update-ratio throughput",
            "x_values": (0.0, 0.0001, 0.001, 0.01),
            "x_labels": ("0", "0.01", "0.1", "1"),
            "x_label": "Updated rows (%)",
            "metric": "qps",
            "y_label": "Median QPS",
            "key": lambda mode, value: (mode, 65536, 16, value),
            "allow_log_y": True,
            "modes": UPDATE_MODES,
        },
    )

    for axis, spec in zip(axes, panel_specs, strict=True):
        positions = tuple(range(len(spec["x_values"])))
        panel_values: list[float] = []
        modes = spec.get("modes", PERFORMANCE_MODES)
        for mode in modes:
            values, lower, upper = _errorbar_values(
                lookup, mode, spec["x_values"], spec["key"], spec["metric"]
            )
            panel_values.extend(values)
            color, marker = MODE_STYLES[mode]
            axis.errorbar(
                positions,
                values,
                yerr=(lower, upper),
                color=color,
                marker=marker,
                linestyle=MODE_LINESTYLES[mode],
                linewidth=1.8,
                markersize=5.5,
                capsize=2.5,
                label=MODE_LABELS[mode],
            )
        log_y = bool(spec["allow_log_y"]) and _use_log_scale(panel_values)
        if log_y:
            axis.set_yscale("log")
        axis.set_title(spec["title"], fontsize=10.5)
        axis.set_xlabel(spec["x_label"])
        axis.set_ylabel(f"{spec['y_label']} (log scale)" if log_y else spec["y_label"])
        axis.set_xticks(positions, spec["x_labels"])
        axis.grid(axis="y", color="#D0D0D0", linewidth=0.7, alpha=0.8)
        axis.spines[["top", "right"]].set_visible(False)

    handles, labels = axes[0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="upper center", ncol=4, frameon=False, bbox_to_anchor=(0.5, 0.995))
    _add_footer(figure, provenance)
    figure.subplots_adjust(left=0.08, right=0.995, top=0.78, bottom=0.22, wspace=0.42)
    pdf_metadata, png_metadata = _figure_metadata(provenance, "Type-2 VectorDB throughput")
    figure.savefig(pdf_path, metadata=pdf_metadata)
    figure.savefig(png_path, dpi=300, metadata=png_metadata)
    plt.close(figure)


def _plot_update_cost(
    records: list[dict[str, str]], provenance: dict[str, str], pdf_path: Path, png_path: Path
) -> None:
    os.environ.setdefault("MPLCONFIGDIR", "/tmp/matplotlib")
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    lookup = _metric_lookup(records)
    ratios = (0.0, 0.0001, 0.001, 0.01)
    positions = tuple(range(len(ratios)))
    labels = ("0", "0.01", "0.1", "1")
    figure, axes = plt.subplots(1, 2, figsize=(7.5, 3.45))
    specs = (
        ("(a) Publication latency", "synchronization_ms", 1.0, "Median synchronization (ms)"),
        ("(b) Data movement", "copied_bytes", 1024.0 * 1024.0, "Median copied data (MiB)"),
    )
    key_builder = lambda mode, ratio: (mode, 65536, 16, ratio)
    for axis, (title, metric, scale, y_label) in zip(axes, specs, strict=True):
        for mode in UPDATE_MODES:
            values, lower, upper = _errorbar_values(lookup, mode, ratios, key_builder, metric, scale)
            color, marker = MODE_STYLES[mode]
            axis.errorbar(
                positions,
                values,
                yerr=(lower, upper),
                color=color,
                marker=marker,
                linewidth=1.8,
                markersize=5.5,
                capsize=2.5,
                label=MODE_LABELS[mode],
            )
        axis.set_title(title, fontsize=10.5)
        axis.set_xlabel("Updated rows (%)")
        axis.set_ylabel(y_label)
        if metric == "copied_bytes":
            axis.set_yscale("symlog", linthresh=0.001)
            axis.set_ylabel(f"{y_label} (symlog)")
        axis.set_xticks(positions, labels)
        axis.grid(axis="y", color="#D0D0D0", linewidth=0.7, alpha=0.8)
        axis.spines[["top", "right"]].set_visible(False)
    handles, legend_labels = axes[0].get_legend_handles_labels()
    figure.legend(handles, legend_labels, loc="upper center", ncol=3, frameon=False, bbox_to_anchor=(0.5, 0.995))
    _add_footer(figure, provenance)
    figure.subplots_adjust(left=0.11, right=0.99, top=0.78, bottom=0.22, wspace=0.35)
    pdf_metadata, png_metadata = _figure_metadata(provenance, "Type-2 VectorDB update cost")
    figure.savefig(pdf_path, metadata=pdf_metadata)
    figure.savefig(png_path, dpi=300, metadata=png_metadata)
    plt.close(figure)


def generate_paper_subset_plots(
    run_dir: Path | str, output_dir: Path | str | None = None
) -> tuple[Path, Path, Path, Path, Path]:
    """Validate a paper-subset run before publishing its CSV and figures."""
    root = Path(run_dir)
    if root.is_file():
        if root.name != "summary.csv":
            raise PlotValidationError("input file must be the run's summary.csv")
        root = root.parent
    manifest, results, summary = _validate_and_load(root)
    aggregate = _build_aggregate(root, manifest, results, summary)
    expected_aggregate_count = len(PAPER_WORKLOADS) * len(PERFORMANCE_MODES)
    if len(aggregate) != expected_aggregate_count:
        raise PlotValidationError(f"aggregated CSV requires exactly {expected_aggregate_count} performance rows")

    destination = Path(output_dir) if output_dir is not None else root / "figures"
    destination.mkdir(parents=True, exist_ok=True)
    csv_path = destination / f"{OUTPUT_STEM}.csv"
    performance_pdf = destination / f"{PERFORMANCE_STEM}.pdf"
    performance_png = destination / f"{PERFORMANCE_STEM}.png"
    update_cost_pdf = destination / f"{UPDATE_COST_STEM}.pdf"
    update_cost_png = destination / f"{UPDATE_COST_STEM}.png"
    with tempfile.TemporaryDirectory(prefix=".type2-vectordb-", dir=destination) as temporary:
        temporary_dir = Path(temporary)
        temporary_csv = temporary_dir / csv_path.name
        temporary_performance_pdf = temporary_dir / performance_pdf.name
        temporary_performance_png = temporary_dir / performance_png.name
        temporary_update_cost_pdf = temporary_dir / update_cost_pdf.name
        temporary_update_cost_png = temporary_dir / update_cost_png.name
        with temporary_csv.open("w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=(*PROVENANCE_FIELDS, *SUMMARY_FIELDNAMES))
            writer.writeheader()
            writer.writerows(aggregate)
        _plot_performance(aggregate, aggregate[0], temporary_performance_pdf, temporary_performance_png)
        _plot_update_cost(aggregate, aggregate[0], temporary_update_cost_pdf, temporary_update_cost_png)
        os.replace(temporary_csv, csv_path)
        os.replace(temporary_performance_pdf, performance_pdf)
        os.replace(temporary_performance_png, performance_png)
        os.replace(temporary_update_cost_pdf, update_cost_pdf)
        os.replace(temporary_update_cost_png, update_cost_png)
    return csv_path, performance_pdf, performance_png, update_cost_pdf, update_cost_png


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path, help="validated run directory or its summary.csv")
    parser.add_argument("--output-dir", type=Path, help="output directory (default: RUN_DIR/figures)")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        outputs = generate_paper_subset_plots(args.run_dir, args.output_dir)
    except PlotValidationError as error:
        raise SystemExit(f"error: {error}") from error
    for output in outputs:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
