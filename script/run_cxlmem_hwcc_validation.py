#!/usr/bin/env python3

"""Fail-closed orchestration for real CXL.mem host-coherence validation."""

from __future__ import annotations

import argparse
import csv
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from typing import Iterable


APPROVED_LENGTH = 2 * 1024 * 1024
EXPECTED_BDF = "0000:64:00.0"
EXPECTED_PCI_ID = "1b00:c002"
EXPECTED_SERIAL = "0x8a0af738c2820407"
EXPECTED_REGION_SIZE = 128 * 1024**3
BENCHMARK_MODES = {"warm-load", "cold-load", "message-passing", "handoff", "fetch-add", "cas"}
BENCHMARK_BACKENDS = {"dram", "cxlmem"}
PMU_GROUPS = {
    "cxl_source": ("mem_load_retired.local_cxl_mem", "ocr.demand_data_rd.local_cxl_mem"),
    "cache_to_cache": (
        "mem_load_l3_hit_retired.xsnp_fwd",
        "ocr.demand_data_rd.l3_hit.snoop_hitm",
    ),
    "device_reads": ("cxl_pmu_mem0.0/m2s_req_memrd/", "cxl_pmu_mem0.0/s2m_drs_memdata/"),
}


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


@dataclass(frozen=True)
class CpuSelection:
    same_numa_a: int
    same_numa_b: int
    remote_numa: int
    package: int
    local_node: int
    remote_node: int


def discover_supported_events(perf_list: str) -> dict[str, str]:
    selected: dict[str, str] = {}
    for group, candidates in PMU_GROUPS.items():
        for event in candidates:
            if re.search(rf"(?<![A-Za-z0-9_.]){re.escape(event)}(?![A-Za-z0-9_.])", perf_list):
                selected[group] = event
                break
        if group not in selected:
            raise ValidationError(f"required PMU group is unavailable: {group}")
    return selected


def parse_perf_stat(output: str, *, separator: str = ";") -> dict[str, float]:
    events: dict[str, float] = {}
    for line in output.splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split(separator)
        if len(fields) < 3:
            continue
        raw_value = fields[0].strip()
        event = fields[2].strip()
        if not event:
            continue
        if raw_value in {"<not supported>", "<not counted>"}:
            raise ValidationError(f"PMU event {event} returned {raw_value}")
        try:
            value = float(raw_value.replace(",", ""))
        except ValueError as error:
            raise ValidationError(f"invalid PMU event value for {event}: {raw_value!r}") from error
        if event in events:
            raise ValidationError(f"duplicate PMU event in perf output: {event}")
        events[event] = value
    if not events:
        raise ValidationError("perf output contains no PMU events")
    return events


def select_topology_cpus(cpu_root: Path, *, allowed_cpus: set[int] | None = None) -> CpuSelection:
    topology_entries: list[tuple[int, int, int, int]] = []
    for cpu_path in sorted(Path(cpu_root).glob("cpu[0-9]*"), key=lambda path: int(path.name[3:])):
        cpu = int(cpu_path.name[3:])
        if allowed_cpus is not None and cpu not in allowed_cpus:
            continue
        online_path = cpu_path / "online"
        if online_path.exists() and _read_int(online_path, f"CPU {cpu} online state") != 1:
            continue
        topology = cpu_path / "topology"
        try:
            package = _read_int(topology / "physical_package_id", f"CPU {cpu} package")
            core = _read_int(topology / "core_id", f"CPU {cpu} core")
        except ValidationError:
            continue
        nodes = [int(path.name[4:]) for path in cpu_path.glob("node[0-9]*")]
        if len(nodes) != 1:
            continue
        topology_entries.append((package, nodes[0], core, cpu))
    packages = sorted({entry[0] for entry in topology_entries})
    if not packages:
        raise ValidationError("CPU topology exposes no online package")
    package = packages[0]
    nodes = sorted({entry[1] for entry in topology_entries if entry[0] == package})
    if len(nodes) < 2:
        raise ValidationError("CPU topology must expose at least two NUMA nodes in one package")
    local_node, remote_node = nodes[:2]
    local_by_core: dict[int, int] = {}
    remote_by_core: dict[int, int] = {}
    for pkg, node, core, cpu in topology_entries:
        if pkg != package:
            continue
        if node == local_node:
            local_by_core.setdefault(core, cpu)
        elif node == remote_node:
            remote_by_core.setdefault(core, cpu)
    local_cores = sorted(local_by_core.items())
    remote_cores = sorted(remote_by_core.items())
    if len(local_cores) < 2:
        raise ValidationError("local NUMA node must expose two distinct physical cores")
    if not remote_cores:
        raise ValidationError("remote NUMA node has no online physical core")
    return CpuSelection(
        same_numa_a=local_cores[0][1],
        same_numa_b=local_cores[1][1],
        remote_numa=remote_cores[0][1],
        package=package,
        local_node=local_node,
        remote_node=remote_node,
    )


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
    advisory_lock_available: bool | None = None,
) -> Attestation:
    if offset != 0 or length <= 0 or length > APPROVED_LENGTH:
        raise ValidationError("mapping exceeds approved 2 MiB boundary [0, 0x200000)")
    if not paths.device.exists():
        raise ValidationError(f"DAX device is missing: {paths.device}")

    holder_ids = tuple(sorted(set(int(pid) for pid in holders)))
    if holder_ids:
        raise ValidationError(f"DAX device has an open holder: {holder_ids}")
    if advisory_lock_available is False:
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

    checks = [
        "approved-range",
        "identity",
        "dvsec-cache-disabled-mem-enabled",
        "committed-ram-region",
        "devdax-resource-match",
        "no-open-holders",
    ]
    checks.append("advisory-lock-available" if advisory_lock_available else "advisory-lock-deferred")
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
        checks=tuple(checks),
    )


def enumerate_device_holders(device: Path, *, ignored_pids: set[int] | None = None) -> tuple[int, ...]:
    ignored = ignored_pids or set()
    try:
        device_stat = device.stat()
    except OSError as error:
        raise ValidationError(f"cannot stat DAX device {device}: {error}") from error
    holders: set[int] = set()
    for process_path in Path("/proc").glob("[0-9]*"):
        pid = int(process_path.name)
        if pid in ignored:
            continue
        fd_root = process_path / "fd"
        try:
            descriptors = list(fd_root.iterdir())
        except OSError:
            continue
        for descriptor in descriptors:
            try:
                opened = descriptor.stat()
            except OSError:
                continue
            if opened.st_rdev == device_stat.st_rdev and opened.st_ino == device_stat.st_ino:
                holders.add(pid)
                break
    return tuple(sorted(holders))


def live_hardware_paths(device: Path, *, dvsec: Path) -> HardwarePaths:
    return HardwarePaths(
        device=device,
        pci=Path("/sys/bus/pci/devices") / EXPECTED_BDF,
        mem=Path("/sys/bus/cxl/devices/mem0"),
        region=Path("/sys/bus/cxl/devices/region0"),
        dax=Path("/sys/bus/dax/devices") / device.name,
        dvsec=dvsec,
    )


def collect_live_attestation(
    device: Path,
    *,
    offset: int,
    length: int,
    holders: tuple[int, ...],
    advisory_lock_available: bool | None,
) -> tuple[Attestation, str]:
    completed = subprocess.run(
        ["lspci", "-vv", "-s", EXPECTED_BDF],
        check=False,
        text=True,
        capture_output=True,
        timeout=10,
    )
    if completed.returncode != 0:
        raise ValidationError(f"lspci failed for {EXPECTED_BDF}: {completed.stderr.strip()}")
    with tempfile.TemporaryDirectory(prefix="cxlmem-hwcc-dvsec-") as temporary:
        dvsec = Path(temporary) / "dvsec.txt"
        dvsec.write_text(completed.stdout)
        attestation = attest_hardware(
            live_hardware_paths(device, dvsec=dvsec),
            offset=offset,
            length=length,
            holders=holders,
            advisory_lock_available=advisory_lock_available,
        )
    return attestation, completed.stdout


def discover_live_pmus() -> tuple[dict[str, str], str]:
    completed = subprocess.run(["perf", "list"], check=False, text=True, capture_output=True, timeout=30)
    if completed.returncode != 0:
        raise ValidationError(f"perf list failed: {completed.stderr.strip()}")
    selected = discover_supported_events(completed.stdout)
    for group, event in selected.items():
        with tempfile.NamedTemporaryFile(prefix=f"cxlmem-hwcc-pmu-{group}-") as output:
            if group == "device_reads":
                command = ["perf", "stat", "-a", "-x;", "-o", output.name, "-e", event, "--", "sleep", "0.01"]
            else:
                command = ["perf", "stat", "-x;", "-o", output.name, "-e", event, "--", "true"]
            probe = subprocess.run(command, check=False, text=True, capture_output=True, timeout=10)
            output.seek(0)
            probe_output = output.read().decode(errors="replace")
        if probe.returncode != 0:
            raise ValidationError(f"PMU event probe failed for {event}: {probe.stderr.strip()}")
        parse_perf_stat(probe_output, separator=";")
    return selected, completed.stdout


def collect_preflight(device: Path, *, offset: int, length: int) -> dict:
    holders = enumerate_device_holders(device)
    attestation, _ = collect_live_attestation(
        device,
        offset=offset,
        length=length,
        holders=holders,
        advisory_lock_available=None,
    )
    cpus = select_topology_cpus(Path("/sys/devices/system/cpu"))
    pmus, _ = discover_live_pmus()
    return {
        "schema": "splash.cxlmem-hwcc-preflight.v1",
        "status": "pass",
        "attestation": asdict(attestation),
        "cpus": asdict(cpus),
        "pmus": pmus,
        "device_opened": False,
        "bytes_written": 0,
        "holders": list(holders),
    }


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


def _git_commit(repository: Path) -> str:
    completed = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=repository, check=False, text=True, capture_output=True, timeout=10
    )
    commit = completed.stdout.strip()
    if completed.returncode != 0 or re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        raise ValidationError(f"cannot determine repository commit: {completed.stderr.strip()}")
    return commit


def _write_json(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def _write_checksums(run_dir: Path, relatives: Iterable[str]) -> None:
    lines = []
    for relative in sorted(set(relatives)):
        digest = hashlib.sha256((run_dir / relative).read_bytes()).hexdigest()
        lines.append(f"{digest}  {relative}")
    (run_dir / "SHA256SUMS").write_text("\n".join(lines) + "\n")


def _capture_command(command: list[str], *, timeout: int = 30) -> str:
    completed = subprocess.run(command, check=False, text=True, capture_output=True, timeout=timeout)
    return (
        f"$ {' '.join(command)}\n"
        f"returncode={completed.returncode}\n"
        f"--- stdout ---\n{completed.stdout}"
        f"--- stderr ---\n{completed.stderr}"
    )


def _benchmark_cases(cpus: CpuSelection) -> list[tuple[str, str, str, int, int]]:
    cases: list[tuple[str, str, str, int, int]] = []
    for backend in ("dram", "cxlmem"):
        for mode in ("warm-load", "cold-load"):
            cases.append((backend, mode, "same-numa", cpus.same_numa_a, cpus.same_numa_b))
        for mode in ("message-passing", "handoff", "fetch-add", "cas"):
            cases.append((backend, mode, "same-numa", cpus.same_numa_a, cpus.same_numa_b))
            cases.append((backend, mode, "cross-numa", cpus.same_numa_a, cpus.remote_numa))
    return cases


def _event_for_case(mode: str, placement: str, pmus: dict[str, str]) -> str:
    if mode in {"warm-load", "cold-load"}:
        return pmus["cxl_source"]
    return pmus["cache_to_cache"]


def _run_measurement(
    *,
    run_dir: Path,
    benchmark: Path,
    device_fd: int,
    length: int,
    iterations: int,
    repetition: int,
    backend: str,
    mode: str,
    placement: str,
    cpu_a: int,
    cpu_b: int,
    pmus: dict[str, str],
) -> tuple[dict, dict[str, float], list[str]]:
    stem = f"r{repetition:02d}-{backend}-{mode}-{placement}"
    raw_dir = run_dir / "raw"
    stdout_path = raw_dir / f"{stem}.stdout.json"
    stderr_path = raw_dir / f"{stem}.stderr.txt"
    cpu_perf_path = raw_dir / f"{stem}.cpu-perf.csv"
    device_perf_path = raw_dir / f"{stem}.device-perf.csv"
    benchmark_command = [
        str(benchmark),
        "--mode",
        mode,
        "--backend",
        backend,
        "--length",
        str(length),
        "--cpu-a",
        str(cpu_a),
        "--cpu-b",
        str(cpu_b),
        "--iterations",
        str(iterations),
    ]
    if backend == "cxlmem":
        benchmark_command.extend(["--device", f"/proc/self/fd/{device_fd}", "--offset", "0"])
    cpu_event = _event_for_case(mode, placement, pmus)
    command = [
        "perf",
        "stat",
        "-a",
        "-x;",
        "-o",
        str(device_perf_path),
        "-e",
        pmus["device_reads"],
        "--",
        "perf",
        "stat",
        "-x;",
        "-o",
        str(cpu_perf_path),
        "-e",
        cpu_event,
        "--",
        *benchmark_command,
    ]
    try:
        completed = subprocess.run(
            command,
            check=False,
            text=True,
            capture_output=True,
            timeout=120,
            pass_fds=(device_fd,),
        )
    except subprocess.TimeoutExpired as error:
        stderr_path.write_text(f"timeout after {error.timeout} seconds\n")
        raise ValidationError(f"measurement timed out without retry: {stem}") from error
    stdout_path.write_text(completed.stdout)
    stderr_path.write_text(completed.stderr)
    if completed.returncode != 0:
        raise ValidationError(f"measurement command failed without retry: {stem} rc={completed.returncode}")
    benchmark_result = parse_benchmark_output(completed.stdout)
    if benchmark_result["errors"] != 0:
        raise ValidationError(f"benchmark correctness failure without retry: {stem}")
    cpu_events = parse_perf_stat(cpu_perf_path.read_text(), separator=";")
    device_events = parse_perf_stat(device_perf_path.read_text(), separator=";")
    events = {**cpu_events, **device_events}
    return benchmark_result, events, [
        str(path.relative_to(run_dir)) for path in (stdout_path, stderr_path, cpu_perf_path, device_perf_path)
    ]


def execute_hardware_run(args: argparse.Namespace) -> Path:
    repository = Path(__file__).resolve().parents[1]
    benchmark = args.benchmark.resolve()
    if not benchmark.is_file():
        raise ValidationError(f"benchmark executable is missing: {benchmark}")
    if os.geteuid() != 0:
        raise ValidationError("hardware execution requires root for DAX and system-wide CXL PMU access")

    preflight = collect_preflight(args.device, offset=args.offset, length=args.length)
    cpus = CpuSelection(**preflight["cpus"])
    pmus = dict(preflight["pmus"])
    commit = _git_commit(repository)
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + f"-{commit[:12]}"
    run_dir = args.output_root / run_id
    raw_dir = run_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=False)
    _write_json(run_dir / "preflight.json", preflight)
    (run_dir / "topology.txt").write_text(_capture_command(["lscpu", "-e=CPU,CORE,SOCKET,NODE,ONLINE"]))
    (run_dir / "cxl-topology.txt").write_text(
        _capture_command(["cxl", "list", "-M", "-m", "-r", "-u"])
        + _capture_command(["daxctl", "list", "-R", "-D", "-u"])
    )
    dmesg_before = _capture_command(["dmesg", "--color=never"], timeout=30)
    aer_before = _capture_command(["lspci", "-vv", "-s", EXPECTED_BDF])
    (run_dir / "dmesg-before.txt").write_text(dmesg_before)
    (run_dir / "aer-before.txt").write_text(aer_before)

    holders = enumerate_device_holders(args.device)
    if holders:
        raise ValidationError(f"DAX device acquired a new open holder before execution: {holders}")
    device_fd = os.open(args.device, os.O_RDWR | os.O_CLOEXEC | os.O_SYNC)
    rows: list[dict[str, str | int | float]] = []
    command_records: list[dict[str, str | int]] = []
    raw_evidence = [
        "preflight.json",
        "topology.txt",
        "cxl-topology.txt",
        "dmesg-before.txt",
        "aer-before.txt",
    ]
    benchmark_results: list[dict] = []
    try:
        try:
            fcntl.flock(device_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise ValidationError(f"DAX device advisory lock is unavailable: {error}") from error
        locked_attestation, dvsec_text = collect_live_attestation(
            args.device,
            offset=args.offset,
            length=args.length,
            holders=(),
            advisory_lock_available=True,
        )
        _write_json(run_dir / "attestation.json", asdict(locked_attestation))
        (run_dir / "dvsec.txt").write_text(dvsec_text)
        raw_evidence.extend(["attestation.json", "dvsec.txt"])

        for repetition in range(args.repetitions):
            for backend, mode, placement, cpu_a, cpu_b in _benchmark_cases(cpus):
                benchmark_result, events, raw_files = _run_measurement(
                    run_dir=run_dir,
                    benchmark=benchmark,
                    device_fd=device_fd,
                    length=args.length,
                    iterations=args.iterations,
                    repetition=repetition,
                    backend=backend,
                    mode=mode,
                    placement=placement,
                    cpu_a=cpu_a,
                    cpu_b=cpu_b,
                    pmus=pmus,
                )
                benchmark_results.append(benchmark_result | {"placement": placement, "repetition": repetition})
                command_name = f"r{repetition:02d}-{backend}-{mode}-{placement}"
                command_records.append({"name": command_name, "returncode": 0})
                raw_evidence.extend(raw_files)
                metrics = {
                    "benchmark.average_ns": benchmark_result["average_ns"],
                    "benchmark.p50_ns": benchmark_result["p50_ns"],
                    "benchmark.p95_ns": benchmark_result["p95_ns"],
                    "benchmark.p99_ns": benchmark_result["p99_ns"],
                }
                for event, value in (metrics | events).items():
                    rows.append(
                        {
                            "backend": backend,
                            "mode": mode,
                            "placement": placement,
                            "repetition": repetition,
                            "event": event,
                            "value": value,
                            "operations": benchmark_result["operations"],
                        }
                    )
    finally:
        try:
            fcntl.flock(device_fd, fcntl.LOCK_UN)
        finally:
            os.close(device_fd)

    dmesg_after = _capture_command(["dmesg", "--color=never"], timeout=30)
    aer_after = _capture_command(["lspci", "-vv", "-s", EXPECTED_BDF])
    (run_dir / "dmesg-after.txt").write_text(dmesg_after)
    (run_dir / "aer-after.txt").write_text(aer_after)
    raw_evidence.extend(["dmesg-after.txt", "aer-after.txt"])
    before_lines = set(dmesg_before.splitlines())
    new_kernel_lines = [line for line in dmesg_after.splitlines() if line not in before_lines]
    concerning = [
        line
        for line in new_kernel_lines
        if re.search(r"AER|Hardware Error|Machine check|MCE|CXL.*(error|fail)", line, re.IGNORECASE)
    ]
    if concerning:
        raise ValidationError(f"new kernel/AER evidence after mutation; no retry allowed: {concerning[:5]}")
    aer_pattern = re.compile(r"^\s*(DevSta|UESta|CESta|CXLSta):")
    aer_status_before = [line.strip() for line in aer_before.splitlines() if aer_pattern.search(line)]
    aer_status_after = [line.strip() for line in aer_after.splitlines() if aer_pattern.search(line)]
    if aer_status_after != aer_status_before:
        raise ValidationError("PCIe/CXL AER status changed after mutation; no retry allowed")

    with (run_dir / "results.csv").open("w", newline="") as handle:
        fieldnames = ["backend", "mode", "placement", "repetition", "event", "value", "operations"]
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    def event_sum(*, backend: str, mode: str, events: set[str], placement: str | None = None) -> float:
        return sum(
            float(row["value"])
            for row in rows
            if row["backend"] == backend
            and row["mode"] == mode
            and row["event"] in events
            and (placement is None or row["placement"] == placement)
        )

    cxl_source_events = {pmus["cxl_source"], pmus["device_reads"]}
    handoff_events = {pmus["cache_to_cache"]}
    warm_media = event_sum(backend="cxlmem", mode="warm-load", events={pmus["device_reads"]})
    cold_media = event_sum(backend="cxlmem", mode="cold-load", events={pmus["device_reads"]})
    load_operations = args.repetitions * args.iterations
    summary = {
        "schema": "splash.cxlmem-hwcc-summary.v1",
        "git_commit": commit,
        "litmus": {"stale": sum(int(item["stale"]) for item in benchmark_results)},
        "atomic": {
            "ticket_errors": sum(int(item["ticket_errors"]) for item in benchmark_results),
            "cas_errors": sum(int(item["cas_errors"]) for item in benchmark_results),
        },
        "pmu": {
            "cold_cxl_reads": event_sum(backend="cxlmem", mode="cold-load", events=cxl_source_events),
            "cache_to_cache_events": sum(
                float(row["value"])
                for row in rows
                if row["backend"] == "cxlmem" and row["mode"] == "handoff" and row["event"] in handoff_events
            ),
            "warm_cxl_media_per_operation": warm_media / load_operations,
            "cold_cxl_media_per_operation": cold_media / load_operations,
            "events": pmus,
        },
        "safety": {
            "approved_length": APPROVED_LENGTH,
            "max_written_offset": max(int(item["max_written_offset"]) for item in benchmark_results),
            "range": "[0, 0x200000)",
        },
        "commands": command_records,
        "raw_evidence": sorted(set(raw_evidence)),
        "repetitions": args.repetitions,
        "iterations": args.iterations,
        "cpus": asdict(cpus),
    }
    if not summary["pmu"]["warm_cxl_media_per_operation"] < summary["pmu"]["cold_cxl_media_per_operation"]:
        raise ValidationError("warm CXL media requests are not lower than cold CXL media requests")
    manifest = {
        "schema": "splash.cxlmem-hwcc-manifest.v1",
        "git_commit": commit,
        "run_id": run_id,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "device": str(args.device),
        "approved_range": "[0, 0x200000)",
        "repetitions": args.repetitions,
        "iterations": args.iterations,
    }
    _write_json(run_dir / "manifest.json", manifest)
    _write_json(run_dir / "summary.json", summary)
    checksummed = ["manifest.json", "results.csv", "summary.json", *summary["raw_evidence"]]
    _write_checksums(run_dir, checksummed)
    validate_run_directory(run_dir, expected_git_commit=commit)
    return run_dir


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
    warm_media = pmu.get("warm_cxl_media_per_operation")
    cold_media = pmu.get("cold_cxl_media_per_operation")
    if (warm_media is None) != (cold_media is None):
        raise ValidationError("warm/cold CXL media comparison is incomplete")
    if warm_media is not None and (
        not isinstance(warm_media, (int, float))
        or not isinstance(cold_media, (int, float))
        or warm_media >= cold_media
    ):
        raise ValidationError("warm CXL media requests must be lower than cold CXL media requests")
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
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--validate-only", type=Path, metavar="RUN_DIR")
    modes.add_argument("--preflight-only", action="store_true")
    modes.add_argument("--execute", action="store_true", help="run the approved hardware experiment")
    parser.add_argument("--device", type=Path, default=Path("/dev/dax0.0"))
    parser.add_argument("--offset", type=int, default=0)
    parser.add_argument("--length", type=int, default=APPROVED_LENGTH)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--iterations", type=int, default=1_000_000)
    parser.add_argument(
        "--benchmark",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "build/microbench/cxlmem_host_cc_validation",
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "artifact/cxlmem_hwcc",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.validate_only is not None:
        result = validate_run_directory(args.validate_only)
        print(json.dumps({"run_dir": str(args.validate_only), "status": result["status"]}, sort_keys=True))
        return 0
    if args.preflight_only:
        result = collect_preflight(args.device, offset=args.offset, length=args.length)
        print(json.dumps(result, sort_keys=True))
        return 0
    if args.repetitions <= 0 or args.iterations <= 0:
        raise ValidationError("repetitions and iterations must be positive")
    if args.offset != 0 or args.length != APPROVED_LENGTH:
        raise ValidationError("execution requires the exact approved 2 MiB range [0, 0x200000)")
    run_dir = execute_hardware_run(args)
    print(json.dumps({"run_dir": str(run_dir), "status": "pass"}, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ValidationError as error:
        print(json.dumps({"status": "fail", "errors": [str(error)]}, sort_keys=True), file=sys.stderr)
        sys.exit(1)
