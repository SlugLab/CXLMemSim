#!/usr/bin/env python3
"""Run and audit the Type-2 VectorDB shared-index experiment."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import shlex
import signal
import socket
import statistics
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]


def configured_path(env_name: str, default: Path) -> Path:
    """Return an explicit experiment path override without changing defaults."""
    return Path(os.environ.get(env_name, str(default))).expanduser()


ARTIFACT_ROOT = REPO_ROOT / "artifact" / "type2_vectordb"
BENCH_DIR = REPO_ROOT / "qemu_integration" / "guest_libcuda"
NATIVE_BINARY = BENCH_DIR / "vectordb_shared_index_native"
GUEST_BINARY = BENCH_DIR / "vectordb_shared_index_guest"
GUEST_LIBCUDA = BENCH_DIR / "libcuda.so.1"
SERVER_BINARY = REPO_ROOT / "build" / "cxlmemsim_server"
QEMU_BINARY = REPO_ROOT / "build" / "qemu-vectordb" / "qemu-system-x86_64"
BASE_IMAGE = configured_path("VECTORDB_BASE_IMAGE", Path("/home/victoryang00/CXLMemSim/build/qemu.img"))
KERNEL_IMAGE = Path("/home/victoryang00/cxl/arch/x86/boot/bzImage")
REAL_LIBCUDA = Path("/usr/lib/x86_64-linux-gnu/libcuda.so.1")
SETUP_SCRIPT = REPO_ROOT / "qemu_integration" / "setup_cxl_numa.sh"

SCHEMA = "splash.vectordb.v1"
MODES = ("type2-hwcc", "software-cc", "full-copy", "native-gpu", "negative-stale")
GUEST_MODES = frozenset(("type2-hwcc", "software-cc", "full-copy"))
WORKLOAD_KEY_FIELDS = ("rows", "dim", "queries", "topk", "update_ratio", "warmup", "seed")
SUMMARY_KEY_FIELDS = ("mode", "rows", "dim", "queries", "topk", "update_ratio")
SUMMARY_METRICS = (
    "end_to_end_ms",
    "update_ms",
    "synchronization_ms",
    "kernel_ms",
    "qps",
    "p50_query_ms",
    "p99_query_ms",
    "copied_bytes",
    "dirty_lines",
)
SUMMARY_FIELDNAMES = (
    *SUMMARY_KEY_FIELDS,
    "epochs",
    *(f"{metric}_{suffix}" for metric in SUMMARY_METRICS for suffix in ("median", "p25", "p75")),
)
SUMMARY_REL_TOLERANCE = 1e-12
SUMMARY_ABS_TOLERANCE = 1e-9
AUDIT_LABELS = {
    "GETS": "gets",
    "GETM": "getm",
    "UPGRADE": "upgrade",
    "PUTS": "puts",
    "PUTM": "putm",
    "Snoop ACK": "snoop_acks",
    "Directory Lines": "directory_lines",
    "Administrative Evict": "directory_evictions",
    "Timeout": "timeout_events",
    "Partial ACK": "partial_ack_events",
    "Stale ACK": "stale_ack_events",
    "Invalid Ownership": "invalid_ownership_events",
}


@dataclass(frozen=True)
class Workload:
    rows: int
    queries: int
    update_ratio: float
    warmup: int
    epochs: int
    seed: int = 1
    dim: int = 128
    topk: int = 10

    @property
    def matrix_bytes(self) -> int:
        return self.rows * self.dim * 4


def _integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _nonnegative_integer(value: Any) -> bool:
    return _integer(value) and value >= 0


def _physical_inventory_match(row: dict[str, Any]) -> bool:
    uuid = row.get("gpu_uuid")
    name = row.get("gpu_name")
    inventory = row.get("gpu_inventory")
    if not isinstance(uuid, str) or not uuid or not isinstance(name, str) or not name:
        return False
    forbidden = ("mock", "simulat", "fake", "software")
    if any(token in name.lower() for token in forbidden) or not isinstance(inventory, list):
        return False
    for gpu in inventory:
        if not isinstance(gpu, dict):
            continue
        inventory_name = gpu.get("name")
        inventory_uuid = gpu.get("uuid")
        driver = gpu.get("driver_version")
        if inventory_uuid == uuid and inventory_name == name and isinstance(driver, str) and driver:
            return not any(token in (inventory_name + inventory_uuid).lower() for token in forbidden)
    return False


def validate_row(row: Any) -> list[str]:
    """Return deterministic proof-gate failures for one enriched result row."""
    if not isinstance(row, dict):
        return ["result row must be a JSON object"]

    errors: list[str] = []
    mode = row.get("mode")
    if row.get("schema") != SCHEMA:
        errors.append(f"schema must be {SCHEMA}")
    if mode not in MODES:
        errors.append("mode is not supported")
    if row.get("protocol_version") != 2:
        errors.append("protocol_version must be 2")
    if not isinstance(row.get("gpu_uuid"), str) or not row.get("gpu_uuid"):
        errors.append("physical GPU UUID is required")
    elif not _physical_inventory_match(row):
        errors.append("physical GPU inventory match is required")
    if row.get("server_commit") != row.get("superproject_commit"):
        errors.append("server commit must match superproject commit")
    if row.get("qemu_commit") != row.get("qemu_gitlink_commit"):
        errors.append("QEMU commit must match superproject gitlink")
    if row.get("topk") != 10:
        errors.append("topk must be 10")

    rows = row.get("rows")
    dim = row.get("dim")
    copied = row.get("copied_bytes")
    dirty = row.get("dirty_lines")
    if not all(_nonnegative_integer(value) for value in (rows, dim, copied, dirty)):
        errors.append("rows, dim, copied_bytes, and dirty_lines must be nonnegative integers")
        return errors

    if mode == "type2-hwcc":
        if row.get("backend") != "qemu-type2-hetgpu":
            errors.append("type2-hwcc backend must be qemu-type2-hetgpu")
        if row.get("host_endpoint") != 0:
            errors.append("type2-hwcc host endpoint must be 0")
        if row.get("device_endpoint") != 1:
            errors.append("type2-hwcc device endpoint must be 1")
        requested = row.get("lines_requested")
        granted = row.get("lines_granted")
        if (
            not _nonnegative_integer(requested)
            or not _nonnegative_integer(granted)
            or requested <= 0
            or granted != requested
        ):
            errors.append("type2-hwcc requires full range grants")
        if not _nonnegative_integer(row.get("partial_grants")) or row.get("partial_grants") != 0:
            errors.append("type2-hwcc observed partial range grants")
        if row.get("grant_evidence") != "device-returned-exact-grant":
            errors.append("type2-hwcc requires device-returned exact grant evidence")
        for field, message in (
            ("timeout_events", "type2-hwcc observed timeout audit events"),
            ("partial_ack_events", "type2-hwcc observed partial ACK audit events"),
            ("stale_ack_events", "type2-hwcc observed stale ACK audit events"),
            ("invalid_ownership_events", "type2-hwcc observed invalid ownership audit events"),
        ):
            if not _nonnegative_integer(row.get(field)) or row.get(field) != 0:
                errors.append(message)
        backing = row.get("backing")
        required_backing = (
            "allocation_count",
            "host_identity",
            "device_identity",
            "size_bytes",
            "mapped_size_bytes",
            "registered",
        )
        if not isinstance(backing, dict) or any(key not in backing for key in required_backing):
            errors.append("type2-hwcc requires one physical backing identity")
        else:
            if backing.get("allocation_count") != 1 or backing.get("registered") is not True:
                errors.append("type2-hwcc requires one physical backing allocation")
            if not backing.get("host_identity") or not backing.get("device_identity"):
                errors.append("type2-hwcc requires one physical backing identity")
            size = backing.get("size_bytes")
            mapped = backing.get("mapped_size_bytes")
            if not _nonnegative_integer(size) or not _nonnegative_integer(mapped) or size <= 0 or size != mapped:
                errors.append("type2-hwcc backing sizes must match")
        if copied != 0:
            errors.append("type2-hwcc copied_bytes must be zero")
        if row.get("correct") is not True:
            errors.append("type2-hwcc correctness must pass")
        if not _nonnegative_integer(row.get("gets")) or row.get("gets") <= 0:
            errors.append("type2-hwcc requires GETS transitions")
        getm = row.get("getm")
        upgrade = row.get("upgrade")
        if not _nonnegative_integer(getm) or not _nonnegative_integer(upgrade) or getm + upgrade <= 0:
            errors.append("type2-hwcc requires GETM or UPGRADE transitions")
        if not _nonnegative_integer(row.get("snoop_acks")) or row.get("snoop_acks") <= 0:
            errors.append("type2-hwcc requires post-update snoop ACKs")
        if row.get("update_ratio") == 0 and row.get("qualification_probe") is not True:
            errors.append("zero-update type2-hwcc requires an unmeasured coherence qualification probe")
    elif mode == "software-cc":
        if (
            row.get("backend") != "qemu-type2-hetgpu"
            or row.get("real_gpu") is not True
            or row.get("correct") is not True
        ):
            errors.append("software-cc requires correct real qemu-type2-hetgpu execution")
        if row.get("update_ratio", 0) > 0 and copied != dirty * 64:
            errors.append("software-cc copied_bytes must equal dirty_lines * 64")
    elif mode == "full-copy":
        if (
            row.get("backend") != "qemu-type2-hetgpu"
            or row.get("real_gpu") is not True
            or row.get("correct") is not True
        ):
            errors.append("full-copy requires correct real qemu-type2-hetgpu execution")
        if copied != rows * dim * 4:
            errors.append("full-copy copied_bytes must equal rows * dim * 4")
    elif mode == "native-gpu":
        if row.get("backend") != "cuda-driver" or row.get("real_gpu") is not True or row.get("correct") is not True:
            errors.append("native-gpu requires correct real cuda-driver execution")
        if copied != 0:
            errors.append("native-gpu copied_bytes must be zero")
    elif mode == "negative-stale":
        if row.get("backend") != "cuda-driver" or row.get("real_gpu") is not True:
            errors.append("negative-stale requires real cuda-driver execution")
        if row.get("stale_observed") is not True or row.get("correct") is not False:
            errors.append("negative-stale requires stale_observed=true and correct=false")
    return errors


def _load_jsonl(path: Path) -> tuple[list[dict[str, Any]], list[str]]:
    rows: list[dict[str, Any]] = []
    errors: list[str] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return [], [f"missing {path.name}"]
    for number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            errors.append(f"{path.name}:{number}: malformed JSON")
            continue
        if not isinstance(row, dict):
            errors.append(f"{path.name}:{number}: result row must be a JSON object")
            continue
        rows.append(row)
    if not rows and not errors:
        errors.append(f"{path.name} contains no result rows")
    return rows, errors


def _workload_key(value: dict[str, Any]) -> tuple[Any, ...]:
    return tuple(value.get(field) for field in WORKLOAD_KEY_FIELDS)


def _validate_manifest_results(manifest: dict[str, Any], rows: list[dict[str, Any]]) -> list[str]:
    errors: list[str] = []
    workloads = manifest.get("workloads")
    if not isinstance(workloads, list) or not workloads:
        return ["manifest workloads must be a nonempty list"]

    expected: dict[tuple[Any, ...], tuple[int, int]] = {}
    for index, workload in enumerate(workloads):
        if not isinstance(workload, dict) or any(field not in workload for field in WORKLOAD_KEY_FIELDS):
            errors.append(f"manifest workload {index} is malformed")
            continue
        epochs = workload.get("epochs")
        if not _integer(epochs) or epochs <= 0:
            errors.append(f"manifest workload {index} epochs must be a positive integer")
            continue
        key = _workload_key(workload)
        if key in expected:
            errors.append(f"manifest workload {index} duplicates an earlier workload key")
            continue
        expected[key] = (index, epochs)

    result_groups: dict[tuple[tuple[Any, ...], Any], list[dict[str, Any]]] = {}
    extra_rows = False
    for row in rows:
        key = _workload_key(row)
        mode = row.get("mode")
        if key not in expected or mode not in MODES:
            extra_rows = True
            continue
        result_groups.setdefault((key, mode), []).append(row)
    if extra_rows:
        errors.append("results contain rows outside manifest workloads")

    for key, (index, epochs) in expected.items():
        for mode in MODES:
            group = result_groups.get((key, mode), [])
            if len(group) != epochs:
                errors.append(f"manifest workload {index} mode {mode} has {len(group)} rows; expected {epochs}")
            epoch_ids = [row.get("epoch") for row in group]
            if not all(_nonnegative_integer(epoch_id) for epoch_id in epoch_ids) or sorted(epoch_ids) != list(
                range(epochs)
            ):
                errors.append(
                    f"manifest workload {index} mode {mode} epoch IDs must be unique and contiguous 0..{epochs - 1}"
                )
            if any(row.get("epochs") != epochs for row in group):
                errors.append(f"manifest workload {index} mode {mode} row epochs must match manifest")
    return errors


def _validate_summary(path: Path, rows: list[dict[str, Any]]) -> list[str]:
    try:
        with path.open(newline="", encoding="utf-8") as source:
            reader = csv.DictReader(source)
            fieldnames = reader.fieldnames
            records = list(reader)
    except (OSError, csv.Error):
        return ["summary.csv is missing or empty"]
    if not fieldnames or not records:
        return ["summary.csv is missing or empty"]
    if tuple(fieldnames) != SUMMARY_FIELDNAMES:
        return ["summary.csv columns do not match the summary contract"]

    try:
        expected_records = _summary_records(rows)
    except (KeyError, TypeError, ValueError):
        return ["results.jsonl lacks numeric fields required to recompute summary.csv"]

    def record_key(record: dict[str, Any]) -> tuple[Any, ...]:
        return (
            record["mode"],
            int(record["rows"]),
            int(record["dim"]),
            int(record["queries"]),
            int(record["topk"]),
            float(record["update_ratio"]),
        )

    try:
        actual_by_key = {record_key(record): record for record in records}
        expected_by_key = {record_key(record): record for record in expected_records}
    except (KeyError, TypeError, ValueError):
        return ["summary.csv contains malformed keys or values"]
    if len(actual_by_key) != len(records) or set(actual_by_key) != set(expected_by_key):
        return ["summary.csv keys do not match results.jsonl groups"]

    errors: list[str] = []
    for key, expected in expected_by_key.items():
        actual = actual_by_key[key]
        mode = str(expected["mode"])
        try:
            if int(actual["epochs"]) != int(expected["epochs"]):
                errors.append(f"summary.csv {mode} epochs does not match results.jsonl")
            for metric in SUMMARY_METRICS:
                for suffix in ("median", "p25", "p75"):
                    field = f"{metric}_{suffix}"
                    actual_value = float(actual[field])
                    expected_value = float(expected[field])
                    if (
                        not math.isfinite(actual_value)
                        or not math.isfinite(expected_value)
                        or not math.isclose(
                            actual_value,
                            expected_value,
                            rel_tol=SUMMARY_REL_TOLERANCE,
                            abs_tol=SUMMARY_ABS_TOLERANCE,
                        )
                    ):
                        errors.append(f"summary.csv {mode} {field} does not match results.jsonl")
        except (KeyError, TypeError, ValueError, OverflowError):
            errors.append(f"summary.csv {mode} contains malformed aggregate values")
    return errors


def validate_run_dir(run_dir: Path | str) -> list[str]:
    root = Path(run_dir)
    rows, errors = _load_jsonl(root / "results.jsonl")
    if errors:
        return errors
    for number, row in enumerate(rows, 1):
        errors.extend(f"results.jsonl:{number}: {message}" for message in validate_row(row))
    if not any(row.get("mode") == "negative-stale" and row.get("stale_observed") is True for row in rows):
        errors.append("run requires negative-stale evidence")
    manifest_path = root / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        errors.append("manifest.json is missing or malformed")
    else:
        if not isinstance(manifest, dict):
            errors.append("manifest.json must contain an object")
            manifest = {}
        if manifest.get("status") != "pass":
            errors.append("manifest status must be pass")
        if manifest.get("budget_seconds") != 28800:
            errors.append("manifest budget_seconds must be 28800")
        if manifest.get("row_count") != len(rows):
            errors.append("manifest row_count must match results.jsonl")
        commands = manifest.get("commands")
        if not isinstance(commands, list) or not commands or not all(isinstance(command, str) for command in commands):
            errors.append("manifest commands must be a nonempty string list")
        commits = manifest.get("commits")
        expected_commits = {
            "superproject": rows[0].get("superproject_commit"),
            "qemu": rows[0].get("qemu_commit"),
            "qemu_gitlink": rows[0].get("qemu_gitlink_commit"),
        }
        if commits != expected_commits:
            errors.append("manifest commits must match validated result rows")
        if manifest.get("gpu_inventory") != rows[0].get("gpu_inventory"):
            errors.append("manifest GPU inventory must match validated result rows")
        errors.extend(_validate_manifest_results(manifest, rows))
        evidence_paths = manifest.get("evidence_paths")
        if not isinstance(evidence_paths, list) or not evidence_paths:
            errors.append("manifest evidence_paths must be nonempty")
        else:
            for relative_path in evidence_paths:
                if not isinstance(relative_path, str) or not relative_path or not (root / relative_path).exists():
                    errors.append(f"manifest evidence path is missing: {relative_path}")
    errors.extend(_validate_summary(root / "summary.csv", rows))
    return errors


def parse_server_evidence(text: str) -> dict[str, int]:
    evidence = {field: 0 for field in AUDIT_LABELS.values()}
    found: set[str] = set()
    for line in text.splitlines():
        for label, field in AUDIT_LABELS.items():
            match = re.search(rf"(?:^|\s){re.escape(label)}:\s*(\d+)\s*$", line)
            if match:
                evidence[field] = int(match.group(1))
                found.add(field)
    missing = sorted(set(AUDIT_LABELS.values()) - found)
    if missing:
        raise ValueError("missing final server counters: " + ", ".join(missing))
    return evidence


def parse_qemu_evidence(text: str, matrix_bytes: int) -> dict[str, Any]:
    legacy_protocol = re.search(
        r"protocol-v2 host endpoint \d+ session 0x[0-9a-f]+, device endpoint \d+ session 0x[0-9a-f]+\s*"
        r"\([^)]+\)\s*$",
        text,
        re.IGNORECASE | re.MULTILINE,
    )
    protocol = re.search(
        r"protocol-v2 host endpoint (\d+) session 0x[0-9a-f]+, device endpoint (\d+) session 0x[0-9a-f]+,\s*"
        r"policy=([a-z-]+)\s*$",
        text,
        re.IGNORECASE | re.MULTILINE,
    )
    pool = re.findall(r"Coherent pool initialized: base=(0x[0-9a-f]+) size=(\d+) MB", text, re.IGNORECASE)
    mapping = re.findall(
        r"Coherent pool GPU mapping: host=(0x[0-9a-f]+) device=(0x[0-9a-f]+) size=(\d+)", text, re.IGNORECASE
    )
    failures = [
        line.strip()
        for line in text.splitlines()
        if re.search(
            r"CXL (?:Type2|hetGPU):.*(?:fail(?:ed|ure)?|error|partial|fallback|fall(?:ing)? back|simulat|"
            r"gpu-mode=1|backend=5)",
            line,
            re.IGNORECASE,
        )
    ]
    if legacy_protocol:
        raise ValueError("QEMU legacy protocol-v2 evidence is not accepted")
    if failures:
        raise ValueError("QEMU reported coherence/GPU errors: " + failures[0])
    if not protocol:
        raise ValueError("QEMU protocol-v2 registration evidence is missing")
    if len(pool) != 1 or len(mapping) != 1:
        raise ValueError("QEMU physical backing identity is missing or ambiguous")
    host_endpoint, device_endpoint, policy = protocol.groups()
    _, pool_mb = pool[0]
    host_identity, device_identity, mapped_size = mapping[0]
    size_bytes = int(pool_mb) * 1024 * 1024
    if policy.lower() != "write-back":
        raise ValueError("QEMU coherence policy is not write-back")
    if int(mapped_size) != size_bytes or matrix_bytes > size_bytes:
        raise ValueError("QEMU coherent backing size does not cover the matrix")
    return {
        "host_endpoint": int(host_endpoint),
        "device_endpoint": int(device_endpoint),
        "backing": {
            "allocation_count": 1,
            "host_identity": host_identity,
            "device_identity": device_identity,
            "size_bytes": size_bytes,
            "mapped_size_bytes": int(mapped_size),
            "registered": True,
        },
    }


def parse_benchmark_jsonl(text: str) -> list[dict[str, Any]]:
    rows = []
    for line_number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as error:
            raise ValueError(f"benchmark stdout line {line_number} is not JSON") from error
        if not isinstance(row, dict):
            raise ValueError(f"benchmark stdout line {line_number} is not an object")
        rows.append(row)
    if not rows:
        raise ValueError("benchmark emitted no JSON rows")
    return rows


def collect_gpu_inventory() -> tuple[list[dict[str, Any]], str]:
    query = [
        "nvidia-smi",
        "--query-gpu=index,name,uuid,driver_version",
        "--format=csv,noheader,nounits",
    ]
    result = subprocess.run(query, check=True, text=True, capture_output=True, timeout=30)
    raw = subprocess.run(["nvidia-smi"], check=True, text=True, capture_output=True, timeout=30).stdout
    inventory = []
    for line in result.stdout.splitlines():
        fields = [field.strip() for field in line.split(",", 3)]
        if len(fields) != 4:
            raise RuntimeError("nvidia-smi returned malformed inventory")
        inventory.append(
            {"index": int(fields[0]), "name": fields[1], "uuid": fields[2], "driver_version": fields[3]}
        )
    if not inventory:
        raise RuntimeError("nvidia-smi returned no physical GPUs")
    return inventory, raw


def git_output(*arguments: str, cwd: Path = REPO_ROOT) -> str:
    return subprocess.run(
        ["git", *arguments], cwd=cwd, check=True, text=True, capture_output=True, timeout=30
    ).stdout.strip()


def command_text(command: Sequence[str]) -> str:
    return shlex.join(str(part) for part in command)


def choose_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def port_is_open(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(0.2)
        return sock.connect_ex(("127.0.0.1", port)) == 0


def stop_process(process: subprocess.Popen[Any] | None, timeout: float = 30.0) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=timeout)


def wait_for_tcp(port: int, process: subprocess.Popen[Any], timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"process exited before TCP port {port} became ready")
        if port_is_open(port):
            return
        time.sleep(0.2)
    raise TimeoutError(f"TCP port {port} did not become ready")


def ssh_options(port: int) -> list[str]:
    return [
        "-p",
        str(port),
        "-o",
        "BatchMode=yes",
        "-o",
        "StrictHostKeyChecking=no",
        "-o",
        "UserKnownHostsFile=/dev/null",
        "-o",
        "ConnectTimeout=3",
        "-o",
        "LogLevel=ERROR",
    ]


def scp_options(port: int) -> list[str]:
    options = ssh_options(port)
    options[0] = "-P"
    return options


def wait_for_ssh(port: int, process: subprocess.Popen[Any], timeout: float = 300.0) -> None:
    deadline = time.monotonic() + timeout
    command = ["ssh", *ssh_options(port), "root@127.0.0.1", "true"]
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("QEMU exited before SSH became ready")
        result = subprocess.run(command, text=True, capture_output=True)
        if result.returncode == 0:
            return
        time.sleep(2.0)
    raise TimeoutError("guest SSH did not become ready")


def benchmark_command(binary: str, mode: str, workload: Workload) -> list[str]:
    return [
        binary,
        "--mode",
        mode,
        "--rows",
        str(workload.rows),
        "--dim",
        str(workload.dim),
        "--queries",
        str(workload.queries),
        "--topk",
        str(workload.topk),
        "--update-ratio",
        f"{workload.update_ratio:.12g}",
        "--warmup",
        str(workload.warmup),
        "--epochs",
        str(workload.epochs),
        "--seed",
        str(workload.seed),
    ]


def server_command(port: int) -> list[str]:
    return [
        str(SERVER_BINARY),
        "--comm-mode=tcp",
        f"--port={port}",
        "--capacity=512",
        "--coherence-v2",
        "--coherence-v2-snoop-timeout-ms=5000",
    ]


def qemu_command(overlay: Path, server_port: int, ssh_port: int, cache_capacity: int) -> list[str]:
    device = ",".join(
        (
            "cxl-type2",
            "bus=type2_rp",
            "id=cxl_type2_0",
            "sn=2",
            "gpu-mode=2",
            "hetgpu-backend=3",
            f"hetgpu-lib={REAL_LIBCUDA}",
            "hetgpu-device=0",
            "coherency-enabled=true",
            "cache-size=128M",
            "mem-size=512M",
            f"cxlmemsim-addr=127.0.0.1",
            f"cxlmemsim-port={server_port}",
            "coherence-v2=on",
            "coherence-v2-host-endpoint=0",
            "coherence-v2-device-endpoint=1",
            f"coherence-v2-cache-capacity={cache_capacity}",
            "coherence-v2-cache-ways=8",
            "coherence-v2-timeout-ms=5000",
            "coherence-v2-write-through=off",
        )
    )
    kernel_args = (
        "root=/dev/vda rw console=ttyS0,115200 nokaslr "
        "systemd.mask=cxl-numa-setup.service systemd.unit=multi-user.target"
    )
    return [
        str(QEMU_BINARY),
        "-accel",
        "kvm",
        "-machine",
        "q35,cxl=on,cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=512M",
        "-cpu",
        "host",
        "-m",
        "4G",
        "-smp",
        "4",
        "-kernel",
        str(KERNEL_IMAGE),
        "-append",
        kernel_args,
        "-drive",
        f"file={overlay},if=none,id=osdisk,format=qcow2",
        "-device",
        "virtio-blk-pci,drive=osdisk,bus=pcie.0",
        "-netdev",
        f"user,id=net0,hostfwd=tcp:127.0.0.1:{ssh_port}-:22",
        "-device",
        "virtio-net-pci,netdev=net0,bus=pcie.0",
        "-device",
        "pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1",
        "-device",
        "cxl-rp,port=0,bus=cxl.1,id=type2_rp,chassis=0,slot=0",
        "-device",
        device,
        "-nographic",
        "-no-reboot",
    ]


def create_run_dir() -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    path = ARTIFACT_ROOT / f"{stamp}-{os.getpid()}"
    path.mkdir(parents=True, exist_ok=False)
    return path


def workload_matrix(smoke: bool, paper_subset: bool) -> list[Workload]:
    if smoke:
        return [Workload(rows=4096, queries=8, update_ratio=1.0 / 4096.0, warmup=0, epochs=1)]
    points: set[tuple[int, int, float]] = set()
    if paper_subset:
        points.update((rows, 16, 0.001) for rows in (16384, 65536, 262144))
        points.update((65536, 16, ratio) for ratio in (0.0, 0.0001, 0.001, 0.01))
        points.update((65536, queries, 0.001) for queries in (1, 16, 64))
    else:
        points.update(
            (rows, queries, ratio)
            for rows in (16384, 65536, 262144)
            for ratio in (0.0, 0.0001, 0.001, 0.01)
            for queries in (1, 16, 64)
        )
    return [Workload(rows, queries, ratio, warmup=5, epochs=10) for rows, queries, ratio in sorted(points)]


def _write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _budget_remaining(deadline: float) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("eight-hour experiment budget exceeded")
    return remaining


def _run_capture(
    command: Sequence[str],
    log_path: Path | None = None,
    cwd: Path = REPO_ROOT,
    timeout: float | None = None,
) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(command, cwd=cwd, text=True, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise TimeoutError(f"command exceeded experiment budget: {command_text(command)}") from error
    if log_path is not None:
        log_path.write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(f"command failed ({result.returncode}): {command_text(command)}")
    return result


def enrich_rows(
    raw_rows: Iterable[dict[str, Any]],
    inventory: list[dict[str, Any]],
    commits: dict[str, str],
    evidence: dict[str, Any],
    workload: Workload,
) -> list[dict[str, Any]]:
    gpu = inventory[0]
    enriched = []
    for raw in raw_rows:
        row = dict(raw)
        grant_fields = ("lines_requested", "lines_granted", "partial_grants", "grant_evidence")
        grant_evidence = None
        if row.get("mode") == "type2-hwcc":
            if any(field not in raw for field in grant_fields):
                raise ValueError("benchmark grant evidence is missing")
            grant_evidence = {field: raw[field] for field in grant_fields}
        row.update(
            {
                "gpu_uuid": gpu["uuid"],
                "gpu_inventory": inventory,
                "protocol_version": 2,
                "superproject_commit": commits["superproject"],
                "server_commit": commits["superproject"],
                "qemu_commit": commits["qemu"],
                "qemu_gitlink_commit": commits["qemu_gitlink"],
            }
        )
        row.update(evidence)
        if grant_evidence is not None:
            row.update(grant_evidence)
        enriched.append(row)
    return enriched


def run_native_mode(
    mode: str,
    workload: Workload,
    point_dir: Path,
    inventory: list[dict[str, Any]],
    commits: dict[str, str],
    commands: list[str],
    deadline: float,
) -> list[dict[str, Any]]:
    command = benchmark_command(str(NATIVE_BINARY), mode, workload)
    commands.append(command_text(command))
    result = _run_capture(command, point_dir / "native.log", BENCH_DIR, _budget_remaining(deadline))
    evidence = {
        "host_endpoint": 0,
        "device_endpoint": 1,
        "gets": 0,
        "getm": 0,
        "upgrade": 0,
        "puts": 0,
        "putm": 0,
        "snoop_acks": 0,
        "directory_lines": 0,
        "directory_evictions": 0,
        "timeout_events": 0,
        "partial_ack_events": 0,
        "stale_ack_events": 0,
        "invalid_ownership_events": 0,
    }
    return enrich_rows(parse_benchmark_jsonl(result.stdout), inventory, commits, evidence, workload)


def run_guest_mode(
    mode: str,
    workload: Workload,
    point_dir: Path,
    inventory: list[dict[str, Any]],
    commits: dict[str, str],
    commands: list[str],
    deadline: float,
) -> list[dict[str, Any]]:
    server_port = choose_free_port()
    ssh_port = choose_free_port()
    while ssh_port == server_port:
        ssh_port = choose_free_port()
    if port_is_open(server_port) or port_is_open(ssh_port):
        raise RuntimeError("collision detected on selected server or SSH port")

    overlay = point_dir / "qemu-overlay.qcow2"
    server_log = point_dir / "server.log"
    qemu_log = point_dir / "qemu.log"
    guest_stdout = point_dir / "guest.jsonl"
    guest_stderr = point_dir / "guest.log"
    cache_capacity = max(64, workload.matrix_bytes)
    create_overlay = ["qemu-img", "create", "-f", "qcow2", "-F", "raw", "-b", str(BASE_IMAGE), str(overlay)]
    server_cmd = server_command(server_port)
    qemu_cmd = qemu_command(overlay, server_port, ssh_port, cache_capacity)
    commands.extend(command_text(command) for command in (create_overlay, server_cmd, qemu_cmd))
    _run_capture(create_overlay, point_dir / "qemu-img.log", timeout=_budget_remaining(deadline))

    server_process = None
    qemu_process = None
    server_handle = server_log.open("w", encoding="utf-8")
    qemu_handle = qemu_log.open("w", encoding="utf-8")
    try:
        server_process = subprocess.Popen(
            server_cmd,
            cwd=REPO_ROOT,
            stdout=server_handle,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
            env={**os.environ, "CXL_BASE_ADDR": "0", "SPDLOG_LEVEL": "info"},
        )
        wait_for_tcp(server_port, server_process, 20.0)
        qemu_process = subprocess.Popen(
            qemu_cmd,
            cwd=REPO_ROOT,
            stdout=qemu_handle,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
            env={**os.environ, "CXL_TRANSPORT_MODE": "tcp"},
        )
        wait_for_ssh(ssh_port, qemu_process, min(300.0, _budget_remaining(deadline)))

        target = "root@127.0.0.1"
        scp_base = ["scp", *scp_options(ssh_port)]
        ssh_base = ["ssh", *ssh_options(ssh_port), target]
        transfers = (
            [*scp_base, str(GUEST_BINARY), f"{target}:/root/vectordb/vectordb_shared_index"],
            [*scp_base, str(GUEST_LIBCUDA), f"{target}:/root/vectordb/libcuda.so.1"],
            [*scp_base, str(SETUP_SCRIPT), f"{target}:/root/vectordb/setup_cxl_numa.sh"],
        )
        mkdir = [*ssh_base, "mkdir -p /root/vectordb"]
        commands.append(command_text(mkdir))
        _run_capture(mkdir, point_dir / "guest-mkdir.log", timeout=_budget_remaining(deadline))
        for transfer_index, transfer in enumerate(transfers):
            commands.append(command_text(transfer))
            _run_capture(
                transfer,
                point_dir / f"scp-{transfer_index}.log",
                timeout=_budget_remaining(deadline),
            )
        setup_remote = (
            "chmod 0755 /root/vectordb/vectordb_shared_index /root/vectordb/setup_cxl_numa.sh && "
            "systemctl mask cxl-numa-setup.service >/dev/null 2>&1 || true; "
            "LOG_FILE=/root/vectordb/cxl-setup.log REGION_SIZE=512M CXL_REGION_TYPE=ram "
            "CXL_CREATE_DAX=1 CXL_DAX_MODE=devdax CXL_TOUCH_DAX=0 CXL_CONFIGURE_NET=0 "
            "/root/vectordb/setup_cxl_numa.sh"
        )
        setup = [*ssh_base, setup_remote]
        commands.append(command_text(setup))
        _run_capture(setup, point_dir / "guest-setup.log", timeout=_budget_remaining(deadline))

        if mode == "type2-hwcc" and workload.update_ratio == 0:
            qualification = Workload(
                rows=workload.rows,
                queries=min(workload.queries, 8),
                update_ratio=1.0 / workload.rows,
                warmup=0,
                epochs=1,
                seed=workload.seed + 1,
            )
            qualification_text = "LD_LIBRARY_PATH=/root/vectordb " + command_text(
                benchmark_command("/root/vectordb/vectordb_shared_index", mode, qualification)
            )
            qualification_command = [*ssh_base, qualification_text]
            commands.append(command_text(qualification_command))
            try:
                qualification_result = subprocess.run(
                    qualification_command,
                    cwd=REPO_ROOT,
                    text=True,
                    capture_output=True,
                    timeout=_budget_remaining(deadline),
                )
            except subprocess.TimeoutExpired as error:
                raise TimeoutError("type2-hwcc qualification probe exceeded experiment budget") from error
            (point_dir / "qualification.jsonl").write_text(qualification_result.stdout, encoding="utf-8")
            (point_dir / "qualification.log").write_text(qualification_result.stderr, encoding="utf-8")
            if qualification_result.returncode != 0:
                raise RuntimeError(f"type2-hwcc qualification probe failed ({qualification_result.returncode})")
            qualification_rows = parse_benchmark_jsonl(qualification_result.stdout)
            if any(row.get("correct") is not True or row.get("copied_bytes") != 0 for row in qualification_rows):
                raise RuntimeError("type2-hwcc qualification probe failed correctness or zero-copy gate")

        remote_benchmark = benchmark_command("/root/vectordb/vectordb_shared_index", mode, workload)
        remote_text = "LD_LIBRARY_PATH=/root/vectordb " + command_text(remote_benchmark)
        guest_command = [*ssh_base, remote_text]
        commands.append(command_text(guest_command))
        try:
            result = subprocess.run(
                guest_command,
                cwd=REPO_ROOT,
                text=True,
                capture_output=True,
                timeout=_budget_remaining(deadline),
            )
        except subprocess.TimeoutExpired as error:
            raise TimeoutError("guest benchmark exceeded experiment budget") from error
        guest_stdout.write_text(result.stdout, encoding="utf-8")
        guest_stderr.write_text(result.stderr, encoding="utf-8")
        if result.returncode != 0:
            raise RuntimeError(f"guest benchmark failed ({result.returncode})")
        raw_rows = parse_benchmark_jsonl(result.stdout)

        poweroff = [*ssh_base, "sync; systemctl poweroff --no-block || poweroff"]
        commands.append(command_text(poweroff))
        try:
            subprocess.run(poweroff, cwd=REPO_ROOT, text=True, capture_output=True, timeout=30)
        except subprocess.TimeoutExpired:
            pass
    finally:
        stop_process(qemu_process)
        stop_process(server_process)
        qemu_handle.close()
        server_handle.close()

    server_evidence = parse_server_evidence(server_log.read_text(encoding="utf-8"))
    qemu_evidence = parse_qemu_evidence(qemu_log.read_text(encoding="utf-8"), workload.matrix_bytes)
    evidence = {
        **server_evidence,
        **qemu_evidence,
        "counter_scope": "fresh-vm-mode-run",
        "qualification_probe": mode == "type2-hwcc" and workload.update_ratio == 0,
    }
    return enrich_rows(raw_rows, inventory, commits, evidence, workload)


def percentile(values: Sequence[float], fraction: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def _summary_records(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = {}
    for row in rows:
        key = tuple(row[field] for field in SUMMARY_KEY_FIELDS)
        groups.setdefault(key, []).append(row)
    records = []
    for key, group in sorted(groups.items()):
        record = dict(zip(SUMMARY_KEY_FIELDS, key))
        record["epochs"] = len(group)
        for metric in SUMMARY_METRICS:
            values = [float(row[metric]) for row in group]
            record[f"{metric}_median"] = statistics.median(values)
            record[f"{metric}_p25"] = percentile(values, 0.25)
            record[f"{metric}_p75"] = percentile(values, 0.75)
        records.append(record)
    return records


def write_summary(rows: list[dict[str, Any]], path: Path) -> None:
    records = _summary_records(rows)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=SUMMARY_FIELDNAMES)
        writer.writeheader()
        writer.writerows(records)


def dry_run(workloads: list[Workload]) -> None:
    print("protocol_version=2 host_endpoint=0 device_endpoint=1 write_policy=write-back")
    print("gpu_mode=real-hetgpu no-sim-fallback")
    print(command_text(["nvidia-smi", "--query-gpu=index,name,uuid,driver_version", "--format=csv,noheader,nounits"]))
    print(command_text(["nvidia-smi"]))
    print(command_text(["lscpu"]))
    print(command_text(["uname", "-a"]))
    print(command_text(["git", "rev-parse", "HEAD"]))
    print(command_text(["git", "rev-parse", "HEAD:lib/qemu"]))
    print(command_text(["git", "-C", str(REPO_ROOT / "lib" / "qemu"), "rev-parse", "HEAD"]))
    print(command_text(["cmake", "--build", str(REPO_ROOT / "build"), "--target", "cxlmemsim_server", "-j"]))
    print(command_text(["make", "-C", str(BENCH_DIR), "vectordb_shared_index_native", "vectordb_shared_index_guest"]))
    for point_index, workload in enumerate(workloads):
        print(f"workload[{point_index}]={json.dumps(asdict(workload), sort_keys=True)}")
        for mode in MODES:
            if mode in GUEST_MODES:
                server_port = 19000 + point_index * 10 + MODES.index(mode)
                ssh_port = 20000 + point_index * 10 + MODES.index(mode)
                overlay = Path(f"RUN_DIR/point-{point_index:03d}-{mode}/qemu-overlay.qcow2")
                print(
                    command_text(
                        ["qemu-img", "create", "-f", "qcow2", "-F", "raw", "-b", str(BASE_IMAGE), str(overlay)]
                    )
                )
                print(command_text(server_command(server_port)))
                print(command_text(qemu_command(overlay, server_port, ssh_port, max(64, workload.matrix_bytes))))
                print(f"ssh -p {ssh_port} root@127.0.0.1 mkdir -p /root/vectordb")
                print(
                    f"scp -P {ssh_port} {GUEST_BINARY} {GUEST_LIBCUDA} {SETUP_SCRIPT} "
                    "root@127.0.0.1:/root/vectordb/"
                )
                print(
                    f"ssh -p {ssh_port} root@127.0.0.1 systemctl mask cxl-numa-setup.service; "
                    "REGION_SIZE=512M /root/vectordb/setup_cxl_numa.sh"
                )
                if mode == "type2-hwcc" and workload.update_ratio == 0:
                    qualification = Workload(
                        rows=workload.rows,
                        queries=min(workload.queries, 8),
                        update_ratio=1.0 / workload.rows,
                        warmup=0,
                        epochs=1,
                        seed=workload.seed + 1,
                    )
                    print(
                        f"ssh -p {ssh_port} root@127.0.0.1 LD_LIBRARY_PATH=/root/vectordb "
                        + command_text(
                            benchmark_command("/root/vectordb/vectordb_shared_index", mode, qualification)
                        )
                        + " # unmeasured post-update coherence qualification"
                    )
                print(
                    f"ssh -p {ssh_port} root@127.0.0.1 LD_LIBRARY_PATH=/root/vectordb "
                    + command_text(benchmark_command("/root/vectordb/vectordb_shared_index", mode, workload))
                )
                print(f"ssh -p {ssh_port} root@127.0.0.1 systemctl poweroff --no-block")
            else:
                print(command_text(benchmark_command(str(NATIVE_BINARY), mode, workload)))


def execute_run(workloads: list[Workload]) -> Path:
    run_dir = create_run_dir()
    commands: list[str] = []
    started = time.monotonic()
    deadline = started + 28800
    inventory, nvidia_raw = collect_gpu_inventory()
    commits = {
        "superproject": git_output("rev-parse", "HEAD"),
        "qemu": git_output("rev-parse", "HEAD", cwd=REPO_ROOT / "lib" / "qemu"),
        "qemu_gitlink": git_output("rev-parse", "HEAD:lib/qemu"),
    }
    inventory_dir = run_dir / "inventory"
    inventory_dir.mkdir()
    _write_json(inventory_dir / "gpu.json", inventory)
    (inventory_dir / "nvidia-smi.txt").write_text(nvidia_raw, encoding="utf-8")
    _run_capture(["uname", "-a"], inventory_dir / "uname.txt", timeout=30)
    _run_capture(["lscpu"], inventory_dir / "lscpu.txt", timeout=30)
    server_build = ["cmake", "--build", str(REPO_ROOT / "build"), "--target", "cxlmemsim_server", "-j"]
    build = ["make", "-C", str(BENCH_DIR), "vectordb_shared_index_native", "vectordb_shared_index_guest"]
    commands.append(command_text(server_build))
    commands.append(command_text(build))
    manifest = {
        "schema": SCHEMA,
        "status": "running",
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "budget_seconds": 28800,
        "commits": commits,
        "gpu_inventory": inventory,
        "inputs": {
            "base_image": str(BASE_IMAGE),
            "kernel_image": str(KERNEL_IMAGE),
            "qemu_binary": str(QEMU_BINARY),
            "server_binary": str(SERVER_BINARY),
            "real_libcuda": str(REAL_LIBCUDA),
        },
        "workloads": [asdict(workload) for workload in workloads],
        "commands": commands,
        "evidence_paths": [
            "inventory/gpu.json",
            "inventory/nvidia-smi.txt",
            "inventory/uname.txt",
            "inventory/lscpu.txt",
        ],
    }
    _write_json(run_dir / "manifest.json", manifest)
    all_rows: list[dict[str, Any]] = []
    results_path = run_dir / "results.jsonl"
    results_path.touch()
    try:
        _run_capture(server_build, run_dir / "server-build.log", timeout=_budget_remaining(deadline))
        _run_capture(build, run_dir / "build.log", timeout=_budget_remaining(deadline))
        for point_index, workload in enumerate(workloads):
            for mode in MODES:
                if time.monotonic() >= deadline:
                    raise TimeoutError("eight-hour experiment budget exceeded")
                point_dir = run_dir / f"point-{point_index:03d}-{mode}"
                point_dir.mkdir()
                if mode in GUEST_MODES:
                    rows = run_guest_mode(mode, workload, point_dir, inventory, commits, commands, deadline)
                else:
                    rows = run_native_mode(mode, workload, point_dir, inventory, commits, commands, deadline)
                for row in rows:
                    errors = validate_row(row)
                    if errors:
                        raise RuntimeError("row validation failed: " + "; ".join(errors))
                all_rows.extend(rows)
                with results_path.open("a", encoding="utf-8") as output:
                    for row in rows:
                        output.write(json.dumps(row, sort_keys=True) + "\n")
                manifest["commands"] = commands
                manifest["evidence_paths"].append(str(point_dir.relative_to(run_dir)))
                _write_json(run_dir / "manifest.json", manifest)

        write_summary(all_rows, run_dir / "summary.csv")
        manifest.update(
            status="pass",
            completed_utc=datetime.now(timezone.utc).isoformat(),
            elapsed_seconds=time.monotonic() - started,
            row_count=len(all_rows),
        )
        _write_json(run_dir / "manifest.json", manifest)
        errors = validate_run_dir(run_dir)
        if errors:
            raise RuntimeError("run validation failed: " + "; ".join(errors))
    except BaseException as error:
        manifest.update(
            status="budget_exceeded" if isinstance(error, TimeoutError) else "failed",
            completed_utc=datetime.now(timezone.utc).isoformat(),
            elapsed_seconds=time.monotonic() - started,
            error=str(error),
            commands=commands,
        )
        _write_json(run_dir / "manifest.json", manifest)
        raise
    return run_dir


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--smoke", action="store_true", help="run the five-mode 4K smoke gate")
    mode.add_argument("--sweep", action="store_true", help="run the paper workload sweep")
    mode.add_argument("--validate-only", metavar="RUN_DIR", type=Path, help="validate an existing run directory")
    parser.add_argument("--paper-subset", action="store_true", help="run only the required paper subset")
    parser.add_argument("--dry-run", action="store_true", help="print commands without executing them")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.paper_subset and not args.sweep:
        raise SystemExit("--paper-subset requires --sweep")
    if args.validate_only is not None:
        errors = validate_run_dir(args.validate_only)
        if errors:
            for error in errors:
                print(error, file=sys.stderr)
            return 1
        print(json.dumps({"status": "pass", "run_dir": str(args.validate_only)}, sort_keys=True))
        return 0
    workloads = workload_matrix(args.smoke, args.paper_subset)
    if args.dry_run:
        dry_run(workloads)
        return 0
    run_dir = execute_run(workloads)
    print(json.dumps({"status": "pass", "run_dir": str(run_dir)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
