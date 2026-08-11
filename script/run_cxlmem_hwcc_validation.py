#!/usr/bin/env python3

"""Fail-closed orchestration for real CXL.mem host-coherence validation."""

from __future__ import annotations

import argparse
import csv
from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from typing import Iterable


APPROVED_LENGTH = 2 * 1024 * 1024
EXPECTED_BDF = "0000:64:00.0"
EXPECTED_PCI_ID = "1b00:c002"
EXPECTED_SERIAL = "0x8a0af738c2820407"
EXPECTED_REGION_SIZE = 128 * 1024**3
BENCHMARK_MODES = {"warm-load", "cold-load", "message-passing", "handoff", "fetch-add", "cas"}
BENCHMARK_BACKENDS = {"dram", "cxlmem"}


class ValidationError(RuntimeError):
    """The requested run cannot meet the hardware proof contract."""


@dataclass(frozen=True)
class HardwarePaths:
    device: Path
    pci: Path
    mem: Path
    region: Path
    dax: Path
    dvsec: Path


@dataclass(frozen=True)
class Attestation:
    bdf: str
    pci_id: str
    serial: str
    firmware: str
    region_resource: int
    region_size: int
    dax_size: int
    offset: int
    length: int
    checks: tuple[str, ...]


def parse_benchmark_output(output: str) -> dict:
    lines = output.splitlines()
    if len(lines) != 1:
        raise ValidationError("benchmark stdout must contain exactly one JSON object")
    try:
        payload = json.loads(lines[0])
    except json.JSONDecodeError as error:
        raise ValidationError("benchmark stdout must contain exactly one JSON object") from error
    if not isinstance(payload, dict):
        raise ValidationError("benchmark output must be a JSON object")
    required = {
        "schema",
        "mode",
        "backend",
        "cpu_a",
        "cpu_b",
        "iterations",
        "operations",
        "errors",
        "flushes_in_hot_path",
        "average_ns",
    }
    missing = sorted(required - payload.keys())
    if missing:
        raise ValidationError(f"benchmark output missing fields: {missing}")
    if payload["schema"] != "splash.cxlmem-hwcc.v1":
        raise ValidationError("benchmark schema mismatch")
    if payload["mode"] not in BENCHMARK_MODES:
        raise ValidationError(f"unknown benchmark mode: {payload['mode']}")
    if payload["backend"] not in BENCHMARK_BACKENDS:
        raise ValidationError(f"unknown benchmark backend: {payload['backend']}")
    for field in ("cpu_a", "cpu_b", "iterations", "operations", "errors", "flushes_in_hot_path"):
        if not isinstance(payload[field], int) or payload[field] < 0:
            raise ValidationError(f"invalid benchmark integer field {field}")
    if not isinstance(payload["average_ns"], (int, float)) or payload["average_ns"] < 0:
        raise ValidationError("invalid benchmark average_ns")
    return payload


def _read(path: Path, description: str) -> str:
    try:
        value = path.read_text().strip()
    except OSError as error:
        raise ValidationError(f"cannot read {description} at {path}: {error}") from error
    if not value:
        raise ValidationError(f"{description} is empty at {path}")
    return value


def _read_int(path: Path, description: str) -> int:
    value = _read(path, description)
    try:
        return int(value, 0)
    except ValueError as error:
        raise ValidationError(f"invalid {description}: {value!r}") from error


def attest_hardware(
    paths: HardwarePaths,
    *,
    offset: int,
    length: int,
    holders: Iterable[int],
    advisory_lock_available: bool = True,
) -> Attestation:
    if offset != 0 or length <= 0 or length > APPROVED_LENGTH:
        raise ValidationError("mapping exceeds approved 2 MiB boundary [0, 0x200000)")
    if not paths.device.exists():
        raise ValidationError(f"DAX device is missing: {paths.device}")

    holder_ids = tuple(sorted(set(int(pid) for pid in holders)))
    if holder_ids:
        raise ValidationError(f"DAX device has an open holder: {holder_ids}")
    if not advisory_lock_available:
        raise ValidationError("DAX device advisory lock is unavailable")

    bdf = paths.pci.name
    vendor = _read(paths.pci / "vendor", "PCI vendor").removeprefix("0x").lower()
    device_id = _read(paths.pci / "device", "PCI device").removeprefix("0x").lower()
    pci_id = f"{vendor}:{device_id}"
    if bdf != EXPECTED_BDF or pci_id != EXPECTED_PCI_ID:
        raise ValidationError(
            f"PCI identity mismatch: expected {EXPECTED_BDF} {EXPECTED_PCI_ID}, found {bdf} {pci_id}"
        )

    serial = _read(paths.mem / "serial", "CXL serial").lower()
    if serial != EXPECTED_SERIAL:
        raise ValidationError(f"CXL serial mismatch: expected {EXPECTED_SERIAL}, found {serial}")
    firmware = _read(paths.mem / "firmware_version", "CXL firmware")

    dvsec = _read(paths.dvsec, "CXL DVSEC dump")
    cache_mem_pattern = re.compile(r"Cache-.*Mem\+", re.IGNORECASE)
    if not any(cache_mem_pattern.search(line) for line in dvsec.splitlines() if "FBCap:" in line):
        raise ValidationError("CXL Flex Bus DVSEC must report Cache- and Mem+")
    if not any(cache_mem_pattern.search(line) for line in dvsec.splitlines() if "CXLCap:" in line):
        raise ValidationError("CXL Device DVSEC must report Cache- and Mem+")

    if _read(paths.region / "mode", "CXL region mode") != "ram":
        raise ValidationError("CXL region mode must be ram")
    if _read_int(paths.region / "commit", "CXL region commit") != 1:
        raise ValidationError("CXL region must be committed")
    region_resource = _read_int(paths.region / "resource", "CXL region resource")
    if region_resource <= 0:
        raise ValidationError("CXL region resource must be nonzero")
    region_size = _read_int(paths.region / "size", "CXL region size")
    if region_size != EXPECTED_REGION_SIZE:
        raise ValidationError(f"CXL region must be exactly 128 GiB, found {region_size} bytes")
    if _read_int(paths.mem / "ram/size", "CXL memdev RAM size") != region_size:
        raise ValidationError("CXL memdev RAM size does not match region size")

    dax_size = _read_int(paths.dax / "size", "DAX size")
    if dax_size != region_size:
        raise ValidationError("DAX size does not match the 128 GiB CXL region")
    if _read_int(paths.dax / "resource", "DAX resource") != region_resource:
        raise ValidationError("DAX resource does not match CXL region resource")
    if _read_int(paths.dax / "target_node", "DAX target node") < 0:
        raise ValidationError("DAX target node is not online")
    dax_alignment = _read_int(paths.dax / "align", "DAX alignment")
    if offset % dax_alignment != 0 or length % dax_alignment != 0:
        raise ValidationError(f"mapping is not aligned to DAX alignment {dax_alignment}")
    if "DRIVER=device_dax" not in _read(paths.dax / "uevent", "DAX uevent"):
        raise ValidationError("DAX device is not bound to device_dax")

    return Attestation(
        bdf=bdf,
        pci_id=pci_id,
        serial=serial,
        firmware=firmware,
        region_resource=region_resource,
        region_size=region_size,
        dax_size=dax_size,
        offset=offset,
        length=length,
        checks=(
            "approved-range",
            "identity",
            "dvsec-cache-disabled-mem-enabled",
            "committed-ram-region",
            "devdax-resource-match",
            "no-open-holders",
            "advisory-lock-available",
        ),
    )


def _load_json(path: Path, description: str) -> dict:
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ValidationError(f"invalid {description} at {path}: {error}") from error
    if not isinstance(value, dict):
        raise ValidationError(f"{description} must be a JSON object")
    return value


def _verify_checksums(run_dir: Path) -> set[str]:
    checksum_path = run_dir / "SHA256SUMS"
    try:
        lines = checksum_path.read_text().splitlines()
    except OSError as error:
        raise ValidationError(f"cannot read checksum manifest: {error}") from error
    if not lines:
        raise ValidationError("checksum manifest is empty")

    verified: set[str] = set()
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  ([^\n]+)", line)
        if not match:
            raise ValidationError(f"invalid checksum entry: {line!r}")
        expected, relative = match.groups()
        relative_path = Path(relative)
        if relative_path.is_absolute() or ".." in relative_path.parts or relative in verified:
            raise ValidationError(f"unsafe or duplicate checksum path: {relative}")
        path = run_dir / relative_path
        try:
            actual = hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError as error:
            raise ValidationError(f"missing checksummed evidence {relative}: {error}") from error
        if actual != expected:
            raise ValidationError(f"checksum mismatch for {relative}: expected {expected}, found {actual}")
        verified.add(relative)
    return verified


def _validate_result_keys(run_dir: Path) -> int:
    try:
        with (run_dir / "results.csv").open(newline="") as handle:
            rows = list(csv.DictReader(handle))
    except OSError as error:
        raise ValidationError(f"cannot read results.csv: {error}") from error
    if not rows:
        raise ValidationError("results.csv contains no measurements")
    key_fields = ("backend", "mode", "placement", "repetition", "event")
    if any(field not in rows[0] for field in key_fields):
        raise ValidationError(f"results.csv is missing key columns {key_fields}")
    keys: set[tuple[str, ...]] = set()
    for row in rows:
        key = tuple(row[field] for field in key_fields)
        if key in keys:
            raise ValidationError(f"duplicate result key: {key}")
        keys.add(key)
        value = row.get("value", "")
        if value in {"", "<not supported>", "<not counted>"}:
            raise ValidationError(f"unsupported-only PMU evidence for result key {key}")
        try:
            float(value.replace(",", ""))
        except ValueError as error:
            raise ValidationError(f"invalid result value {value!r} for {key}") from error
    return len(rows)


def validate_run_directory(run_dir: Path, *, expected_git_commit: str | None = None) -> dict:
    run_dir = Path(run_dir)
    verified = _verify_checksums(run_dir)
    required = {"manifest.json", "results.csv", "summary.json"}
    missing = required - verified
    if missing:
        raise ValidationError(f"checksum manifest omits required files: {sorted(missing)}")

    manifest = _load_json(run_dir / "manifest.json", "manifest")
    summary = _load_json(run_dir / "summary.json", "summary")
    if manifest.get("schema") != "splash.cxlmem-hwcc-manifest.v1":
        raise ValidationError("manifest schema mismatch")
    if summary.get("schema") != "splash.cxlmem-hwcc-summary.v1":
        raise ValidationError("summary schema mismatch")

    commit = summary.get("git_commit")
    if commit != manifest.get("git_commit") or (expected_git_commit is not None and commit != expected_git_commit):
        raise ValidationError("git commit mismatch between run, manifest, and validator")
    if not isinstance(commit, str) or re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        raise ValidationError("git commit is not a full 40-character object ID")

    raw_evidence = summary.get("raw_evidence")
    if not isinstance(raw_evidence, list) or not raw_evidence:
        raise ValidationError("raw evidence list is missing or empty")
    for relative in raw_evidence:
        if relative not in verified:
            raise ValidationError(f"raw evidence is not checksummed: {relative}")

    commands = summary.get("commands")
    if not isinstance(commands, list) or not commands:
        raise ValidationError("command evidence is missing")
    failed_commands = [item.get("name", "unknown") for item in commands if item.get("returncode") != 0]
    if failed_commands:
        raise ValidationError(f"command failures invalidate run: {failed_commands}")

    litmus = summary.get("litmus", {})
    atomic = summary.get("atomic", {})
    pmu = summary.get("pmu", {})
    safety = summary.get("safety", {})
    if litmus.get("stale") != 0:
        raise ValidationError(f"stale litmus observations: {litmus.get('stale')}")
    if atomic.get("ticket_errors") != 0:
        raise ValidationError(f"ticket atomic errors: {atomic.get('ticket_errors')}")
    if atomic.get("cas_errors") != 0:
        raise ValidationError(f"CAS atomic errors: {atomic.get('cas_errors')}")
    if not isinstance(pmu.get("cold_cxl_reads"), (int, float)) or pmu["cold_cxl_reads"] <= 0:
        raise ValidationError("cold CXL source PMU evidence is absent")
    if not isinstance(pmu.get("cache_to_cache_events"), (int, float)) or pmu["cache_to_cache_events"] <= 0:
        raise ValidationError("cache-to-cache PMU evidence is absent")
    if safety.get("approved_length") != APPROVED_LENGTH:
        raise ValidationError("run does not record the approved 2 MiB length")
    max_written = safety.get("max_written_offset")
    if not isinstance(max_written, int) or max_written < 0 or max_written > APPROVED_LENGTH:
        raise ValidationError(f"write exceeded approved 2 MiB boundary: {max_written}")

    row_count = _validate_result_keys(run_dir)
    validation = {
        "schema": "splash.cxlmem-hwcc-validation.v1",
        "status": "pass",
        "git_commit": commit,
        "checksummed_files": len(verified),
        "result_rows": row_count,
        "proof_gates": {
            "litmus": "pass",
            "atomics": "pass",
            "cxl_source_pmu": "pass",
            "cache_to_cache_pmu": "pass",
            "write_boundary": "pass",
        },
    }
    temporary = run_dir / ".validation.json.tmp"
    temporary.write_text(json.dumps(validation, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, run_dir / "validation.json")
    return validation


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validate-only", type=Path, metavar="RUN_DIR")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.validate_only is None:
        raise ValidationError("one execution mode is required; currently supported: --validate-only RUN_DIR")
    result = validate_run_directory(args.validate_only)
    print(json.dumps({"run_dir": str(args.validate_only), "status": result["status"]}, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ValidationError as error:
        print(json.dumps({"status": "fail", "errors": [str(error)]}, sort_keys=True), file=sys.stderr)
        sys.exit(1)
