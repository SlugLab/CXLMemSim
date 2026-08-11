#!/usr/bin/env python3

import csv
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from script.run_cxlmem_hwcc_validation import (
    APPROVED_LENGTH,
    HardwarePaths,
    PMU_GROUPS,
    ValidationError,
    attest_hardware,
    discover_supported_events,
    parse_benchmark_output,
    parse_perf_stat,
    select_topology_cpus,
    validate_run_directory,
)


EXPECTED_COMMIT = "0123456789abcdef0123456789abcdef01234567"


def write_hardware_fixture(root: Path) -> HardwarePaths:
    pci = root / "sys/bus/pci/devices/0000:64:00.0"
    mem = root / "sys/bus/cxl/devices/mem0"
    region = root / "sys/bus/cxl/devices/region0"
    dax = root / "sys/bus/dax/devices/dax0.0"
    device = root / "dev/dax0.0"
    for directory in (pci, mem, region, dax, device.parent):
        directory.mkdir(parents=True, exist_ok=True)
    device.touch()

    (pci / "vendor").write_text("0x1b00\n")
    (pci / "device").write_text("0xc002\n")
    (pci / "dvsec.txt").write_text(
        "FBCap:\tCache- IO+ Mem+\n"
        "CXLCap:\tCache- IO+ Mem+ MemHWInit+ HDMCount 1\n"
    )
    (mem / "serial").write_text("0x8a0af738c2820407\n")
    (mem / "firmware_version").write_text("20.00.0.0609.00\n")
    (mem / "ram").mkdir()
    (mem / "ram/size").write_text("0x2000000000\n")
    (region / "mode").write_text("ram\n")
    (region / "commit").write_text("1\n")
    (region / "resource").write_text("0x2080000000\n")
    (region / "size").write_text("0x2000000000\n")
    (dax / "size").write_text("137438953472\n")
    (dax / "resource").write_text("0x2080000000\n")
    (dax / "target_node").write_text("2\n")
    (dax / "align").write_text("4096\n")
    (dax / "uevent").write_text("DEVNAME=dax0.0\nDRIVER=device_dax\n")
    return HardwarePaths(device=device, pci=pci, mem=mem, region=region, dax=dax, dvsec=pci / "dvsec.txt")


def write_run_fixture(root: Path) -> Path:
    run_dir = root / "run"
    raw = run_dir / "raw"
    raw.mkdir(parents=True)
    (raw / "cold-cxl-perf.csv").write_text("100,,mem_load_retired.local_cxl_mem\n")
    rows = [
        {
            "backend": "cxlmem",
            "mode": "cold-load",
            "placement": "same-numa",
            "repetition": "0",
            "event": "mem_load_retired.local_cxl_mem",
            "value": "100",
        },
        {
            "backend": "cxlmem",
            "mode": "handoff",
            "placement": "cross-numa",
            "repetition": "0",
            "event": "mem_load_l3_miss_retired.remote_hitm",
            "value": "20",
        },
    ]
    with (run_dir / "results.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    summary = {
        "schema": "splash.cxlmem-hwcc-summary.v1",
        "git_commit": EXPECTED_COMMIT,
        "litmus": {"stale": 0},
        "atomic": {"ticket_errors": 0, "cas_errors": 0},
        "pmu": {"cold_cxl_reads": 100, "cache_to_cache_events": 20},
        "safety": {"max_written_offset": APPROVED_LENGTH, "approved_length": APPROVED_LENGTH},
        "commands": [{"name": "cold-cxl", "returncode": 0}],
        "raw_evidence": ["raw/cold-cxl-perf.csv"],
    }
    (run_dir / "summary.json").write_text(json.dumps(summary, sort_keys=True) + "\n")
    (run_dir / "manifest.json").write_text(
        json.dumps({"schema": "splash.cxlmem-hwcc-manifest.v1", "git_commit": EXPECTED_COMMIT}) + "\n"
    )
    checksummed = ["manifest.json", "results.csv", "summary.json", "raw/cold-cxl-perf.csv"]
    with (run_dir / "SHA256SUMS").open("w") as handle:
        for relative in checksummed:
            digest = hashlib.sha256((run_dir / relative).read_bytes()).hexdigest()
            handle.write(f"{digest}  {relative}\n")
    return run_dir


class HardwareAttestationTest(unittest.TestCase):
    def test_accepts_exact_read_only_hardware_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = write_hardware_fixture(Path(temporary))
            attestation = attest_hardware(paths, offset=0, length=APPROVED_LENGTH, holders=())
        self.assertEqual(attestation.bdf, "0000:64:00.0")
        self.assertEqual(attestation.pci_id, "1b00:c002")
        self.assertEqual(attestation.serial, "0x8a0af738c2820407")
        self.assertEqual(attestation.region_size, 128 * 1024**3)
        self.assertEqual(attestation.length, APPROVED_LENGTH)
        self.assertIn("dvsec-cache-disabled-mem-enabled", attestation.checks)

    def test_rejects_mapping_beyond_two_mib(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = write_hardware_fixture(Path(temporary))
            with self.assertRaisesRegex(ValidationError, "approved 2 MiB"):
                attest_hardware(paths, offset=0, length=APPROVED_LENGTH + 1, holders=())

    def test_rejects_nonzero_offset(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = write_hardware_fixture(Path(temporary))
            with self.assertRaisesRegex(ValidationError, "approved 2 MiB"):
                attest_hardware(paths, offset=4096, length=4096, holders=())

    def test_rejects_each_identity_or_mode_mismatch(self):
        mutations = {
            "vendor": ("pci", "vendor", "0xffff\n", "PCI identity"),
            "device": ("pci", "device", "0xffff\n", "PCI identity"),
            "serial": ("mem", "serial", "0xdeadbeef\n", "serial"),
            "firmware": ("mem", "firmware_version", "\n", "firmware"),
            "dvsec": ("pci", "dvsec.txt", "FBCap: Cache+ IO+ Mem+\n", "Cache-.*Mem\\+"),
            "region-mode": ("region", "mode", "pmem\n", "region mode"),
            "region-commit": ("region", "commit", "0\n", "committed"),
            "region-resource": ("region", "resource", "0x0\n", "resource"),
            "region-size": ("region", "size", "0x1000\n", "128 GiB"),
            "dax-size": ("dax", "size", "4096\n", "DAX size"),
            "dax-resource": ("dax", "resource", "0x2080100000\n", "DAX resource"),
            "dax-driver": ("dax", "uevent", "DRIVER=other\n", "device_dax"),
        }
        for name, (base, relative, value, message) in mutations.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                paths = write_hardware_fixture(Path(temporary))
                (getattr(paths, base) / relative).write_text(value)
                with self.assertRaisesRegex(ValidationError, message):
                    attest_hardware(paths, offset=0, length=APPROVED_LENGTH, holders=())

    def test_rejects_open_holder_and_unavailable_advisory_lock(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = write_hardware_fixture(Path(temporary))
            with self.assertRaisesRegex(ValidationError, "open holder"):
                attest_hardware(paths, offset=0, length=APPROVED_LENGTH, holders=(1234,))
            with self.assertRaisesRegex(ValidationError, "advisory lock"):
                attest_hardware(
                    paths,
                    offset=0,
                    length=APPROVED_LENGTH,
                    holders=(),
                    advisory_lock_available=False,
                )


class ArtifactValidationTest(unittest.TestCase):
    def test_complete_fixture_passes_and_writes_only_validation(self):
        with tempfile.TemporaryDirectory() as temporary:
            run_dir = write_run_fixture(Path(temporary))
            before = {path.relative_to(run_dir): path.read_bytes() for path in run_dir.rglob("*") if path.is_file()}
            validation = validate_run_directory(run_dir, expected_git_commit=EXPECTED_COMMIT)
            after = {path.relative_to(run_dir): path.read_bytes() for path in run_dir.rglob("*") if path.is_file()}
        self.assertEqual(validation["status"], "pass")
        self.assertEqual(set(after) - set(before), {Path("validation.json")})
        for path, data in before.items():
            self.assertEqual(after[path], data)

    def test_proof_gates_fail_independently(self):
        mutations = (
            (lambda summary: summary["litmus"].update(stale=1), "stale"),
            (lambda summary: summary["atomic"].update(ticket_errors=1), "ticket"),
            (lambda summary: summary["pmu"].update(cold_cxl_reads=0), "cold CXL"),
            (lambda summary: summary["pmu"].update(cache_to_cache_events=0), "cache-to-cache"),
            (lambda summary: summary["safety"].update(max_written_offset=APPROVED_LENGTH + 1), "2 MiB"),
            (lambda summary: summary["commands"][0].update(returncode=1), "command"),
            (lambda summary: summary.update(raw_evidence=[]), "raw evidence"),
            (lambda summary: summary.update(git_commit="f" * 40), "git commit"),
        )
        for mutate, message in mutations:
            with self.subTest(message=message), tempfile.TemporaryDirectory() as temporary:
                run_dir = write_run_fixture(Path(temporary))
                summary_path = run_dir / "summary.json"
                summary = json.loads(summary_path.read_text())
                mutate(summary)
                summary_path.write_text(json.dumps(summary, sort_keys=True) + "\n")
                self._refresh_checksum(run_dir, "summary.json")
                with self.assertRaisesRegex(ValidationError, message):
                    validate_run_directory(run_dir, expected_git_commit=EXPECTED_COMMIT)

    def test_rejects_checksum_mismatch_and_duplicate_result_key(self):
        with tempfile.TemporaryDirectory() as temporary:
            run_dir = write_run_fixture(Path(temporary))
            (run_dir / "summary.json").write_text("{}\n")
            with self.assertRaisesRegex(ValidationError, "checksum"):
                validate_run_directory(run_dir, expected_git_commit=EXPECTED_COMMIT)

        with tempfile.TemporaryDirectory() as temporary:
            run_dir = write_run_fixture(Path(temporary))
            results = (run_dir / "results.csv").read_text().splitlines()
            (run_dir / "results.csv").write_text("\n".join(results + [results[1]]) + "\n")
            self._refresh_checksum(run_dir, "results.csv")
            with self.assertRaisesRegex(ValidationError, "duplicate result key"):
                validate_run_directory(run_dir, expected_git_commit=EXPECTED_COMMIT)

    @staticmethod
    def _refresh_checksum(run_dir: Path, relative: str):
        lines = []
        for line in (run_dir / "SHA256SUMS").read_text().splitlines():
            _, name = line.split("  ", 1)
            if name == relative:
                digest = hashlib.sha256((run_dir / relative).read_bytes()).hexdigest()
                line = f"{digest}  {relative}"
            lines.append(line)
        (run_dir / "SHA256SUMS").write_text("\n".join(lines) + "\n")


class BenchmarkOutputTest(unittest.TestCase):
    def test_accepts_single_complete_benchmark_object(self):
        payload = {
            "schema": "splash.cxlmem-hwcc.v1",
            "mode": "handoff",
            "backend": "cxlmem",
            "cpu_a": 0,
            "cpu_b": 43,
            "iterations": 1000,
            "operations": 2000,
            "errors": 0,
            "flushes_in_hot_path": 0,
            "average_ns": 42.5,
        }
        self.assertEqual(parse_benchmark_output(json.dumps(payload) + "\n"), payload)

    def test_rejects_non_json_extra_output(self):
        valid = json.dumps(
            {
                "schema": "splash.cxlmem-hwcc.v1",
                "mode": "warm-load",
                "backend": "dram",
                "cpu_a": 0,
                "cpu_b": 1,
                "iterations": 1,
                "operations": 1,
                "errors": 0,
                "flushes_in_hot_path": 0,
                "average_ns": 1.0,
            }
        )
        for output in (f"debug\n{valid}\n", f"{valid}\ntrailer\n", f"{valid}\n{valid}\n"):
            with self.subTest(output=output), self.assertRaisesRegex(ValidationError, "exactly one JSON"):
                parse_benchmark_output(output)

    def test_rejects_unknown_or_incomplete_benchmark_object(self):
        base = {
            "schema": "splash.cxlmem-hwcc.v1",
            "mode": "handoff",
            "backend": "dram",
            "cpu_a": 0,
            "cpu_b": 1,
            "iterations": 1,
            "operations": 2,
            "errors": 0,
            "flushes_in_hot_path": 0,
            "average_ns": 1.0,
        }
        for field, value in (("schema", "bad"), ("mode", "bad"), ("backend", "bad")):
            payload = dict(base)
            payload[field] = value
            with self.subTest(field=field), self.assertRaises(ValidationError):
                parse_benchmark_output(json.dumps(payload))
        for missing in base:
            payload = dict(base)
            payload.pop(missing)
            with self.subTest(missing=missing), self.assertRaisesRegex(ValidationError, "missing"):
                parse_benchmark_output(json.dumps(payload))


class PerfAndTopologyTest(unittest.TestCase):
    def test_discovers_first_supported_event_in_each_group(self):
        perf_list = """
          mem_load_retired.local_cxl_mem
          ocr.demand_data_rd.l3_hit.snoop_hitm
          cxl_pmu_mem0.0/m2s_req_memrd/
        """
        selected = discover_supported_events(perf_list)
        self.assertEqual(selected["cxl_source"], "mem_load_retired.local_cxl_mem")
        self.assertEqual(selected["cache_to_cache"], "ocr.demand_data_rd.l3_hit.snoop_hitm")
        self.assertEqual(selected["device_reads"], "cxl_pmu_mem0.0/m2s_req_memrd/")

    def test_rejects_missing_pmu_group(self):
        with self.assertRaisesRegex(ValidationError, "device_reads"):
            discover_supported_events(
                "\n".join(
                    candidates[0] for group, candidates in PMU_GROUPS.items() if group != "device_reads"
                )
            )

    def test_parses_perf_csv_and_rejects_unsupported_values(self):
        parsed = parse_perf_stat(
            "1,234;;mem_load_retired.local_cxl_mem;100.00;100.00;\n"
            "20;;mem_load_l3_miss_retired.remote_hitm;100.00;100.00;\n",
            separator=";",
        )
        self.assertEqual(parsed["mem_load_retired.local_cxl_mem"], 1234.0)
        self.assertEqual(parsed["mem_load_l3_miss_retired.remote_hitm"], 20.0)
        for marker in ("<not supported>", "<not counted>"):
            with self.subTest(marker=marker), self.assertRaisesRegex(ValidationError, "PMU event"):
                parse_perf_stat(f"{marker};;event;;\n", separator=";")

    def test_selects_distinct_same_numa_cores_and_remote_numa_core(self):
        with tempfile.TemporaryDirectory() as temporary:
            cpu_root = Path(temporary)
            topology = {0: (0, 0, 0), 1: (0, 0, 1), 2: (0, 0, 0), 43: (0, 1, 64)}
            for cpu, (package, node, core) in topology.items():
                directory = cpu_root / f"cpu{cpu}/topology"
                directory.mkdir(parents=True)
                (directory / "physical_package_id").write_text(f"{package}\n")
                (directory / "core_id").write_text(f"{core}\n")
                (cpu_root / f"cpu{cpu}/online").write_text("1\n")
                (cpu_root / f"cpu{cpu}/node{node}").mkdir()
            selected = select_topology_cpus(cpu_root, allowed_cpus={0, 1, 2, 43})
        self.assertEqual(selected.same_numa_a, 0)
        self.assertEqual(selected.same_numa_b, 1)
        self.assertEqual(selected.remote_numa, 43)

    def test_topology_rejects_single_package_or_sibling_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            cpu_root = Path(temporary)
            for cpu in (0, 1):
                directory = cpu_root / f"cpu{cpu}/topology"
                directory.mkdir(parents=True)
                (directory / "physical_package_id").write_text("0\n")
                (directory / "core_id").write_text("0\n")
                (cpu_root / f"cpu{cpu}/node0").mkdir()
            with self.assertRaisesRegex(ValidationError, "two NUMA nodes"):
                select_topology_cpus(cpu_root, allowed_cpus={0, 1})


if __name__ == "__main__":
    unittest.main(verbosity=2)
