import contextlib
import csv
import io
import json
import os
import signal
import tempfile
import unittest
from pathlib import Path
from subprocess import TimeoutExpired
from unittest import mock

from script.run_type2_vectordb import (
    Workload,
    configured_path,
    enrich_rows,
    main,
    parse_benchmark_jsonl,
    parse_qemu_evidence,
    parse_server_evidence,
    qemu_command,
    stop_process,
    validate_row,
    validate_run_dir,
    write_summary,
)


def valid_row(mode="type2-hwcc", **overrides):
    row = {
        "schema": "splash.vectordb.v1",
        "mode": mode,
        "backend": "qemu-type2-hetgpu"
        if mode in ("type2-hwcc", "software-cc", "full-copy")
        else "cuda-driver",
        "gpu_name": "NVIDIA RTX PRO 6000 Blackwell Server Edition",
        "gpu_uuid": "GPU-01234567-89ab-cdef-0123-456789abcdef",
        "gpu_inventory": [
            {
                "index": 0,
                "name": "NVIDIA RTX PRO 6000 Blackwell Server Edition",
                "uuid": "GPU-01234567-89ab-cdef-0123-456789abcdef",
                "driver_version": "595.84",
            }
        ],
        "real_gpu": True,
        "rows": 4096,
        "dim": 128,
        "queries": 8,
        "topk": 10,
        "update_ratio": 1.0 / 4096.0,
        "warmup": 0,
        "epochs": 1,
        "epoch": 0,
        "seed": 1,
        "copied_bytes": 0,
        "dirty_lines": 8,
        "correct": True,
        "stale_observed": False,
        "protocol_version": 2,
        "host_endpoint": 0,
        "device_endpoint": 1,
        "server_commit": "a" * 40,
        "superproject_commit": "a" * 40,
        "qemu_commit": "b" * 40,
        "qemu_gitlink_commit": "b" * 40,
        "lines_requested": 32768,
        "lines_granted": 32768,
        "partial_grants": 0,
        "grant_evidence": "device-returned-exact-grant",
        "end_to_end_ms": 1.0,
        "update_ms": 0.1,
        "synchronization_ms": 0.2,
        "kernel_ms": 0.7,
        "qps": 8000.0,
        "p50_query_ms": 0.6,
        "p99_query_ms": 0.7,
        "gets": 32768,
        "getm": 8,
        "upgrade": 0,
        "puts": 0,
        "putm": 0,
        "snoop_acks": 8,
        "directory_lines": 32768,
        "directory_evictions": 0,
        "timeout_events": 0,
        "partial_ack_events": 0,
        "stale_ack_events": 0,
        "invalid_ownership_events": 0,
        "backing": {
            "allocation_count": 1,
            "host_identity": "0x7f0000000000",
            "device_identity": "0x7f0000000000",
            "size_bytes": 2097152,
            "mapped_size_bytes": 2097152,
            "registered": True,
        },
    }
    if mode == "software-cc":
        row["copied_bytes"] = row["dirty_lines"] * 64
    elif mode == "full-copy":
        row["copied_bytes"] = row["rows"] * row["dim"] * 4
    elif mode == "negative-stale":
        row.update(correct=False, stale_observed=True, observed_label=0, expected_label=1410)
    if overrides:
        row.update(overrides)
    return row


class ValidateRowTests(unittest.TestCase):
    def assert_rejected(self, field, value, message, mode="type2-hwcc"):
        row = valid_row(mode)
        row[field] = value
        self.assertIn(message, validate_row(row))

    def test_accepts_each_valid_mode(self):
        for mode in ("type2-hwcc", "software-cc", "full-copy", "native-gpu", "negative-stale"):
            with self.subTest(mode=mode):
                self.assertEqual([], validate_row(valid_row(mode)))

    def test_rejects_schema_and_protocol_v1(self):
        self.assert_rejected("schema", "splash.vectordb.v0", "schema must be splash.vectordb.v1")
        self.assert_rejected("protocol_version", 1, "protocol_version must be 2")

    def test_rejects_mock_or_simulated_gpu(self):
        self.assert_rejected("gpu_name", "NVIDIA mock GPU", "physical GPU inventory match is required")
        self.assert_rejected("backend", "simulation", "type2-hwcc backend must be qemu-type2-hetgpu")

    def test_rejects_missing_uuid_or_inventory_mismatch(self):
        self.assert_rejected("gpu_uuid", "", "physical GPU UUID is required")
        self.assert_rejected(
            "gpu_uuid",
            "GPU-ffffffff-ffff-ffff-ffff-ffffffffffff",
            "physical GPU inventory match is required",
        )

    def test_rejects_mismatched_server_and_qemu_commits(self):
        self.assert_rejected("server_commit", "c" * 40, "server commit must match superproject commit")
        self.assert_rejected("qemu_commit", "d" * 40, "QEMU commit must match superproject gitlink")

    def test_rejects_partial_grants_and_bad_audit_events(self):
        self.assert_rejected("lines_granted", 32767, "type2-hwcc requires full range grants")
        self.assert_rejected("partial_grants", 1, "type2-hwcc observed partial range grants")
        for field, message in (
            ("timeout_events", "type2-hwcc observed timeout audit events"),
            ("partial_ack_events", "type2-hwcc observed partial ACK audit events"),
            ("stale_ack_events", "type2-hwcc observed stale ACK audit events"),
            ("invalid_ownership_events", "type2-hwcc observed invalid ownership audit events"),
        ):
            with self.subTest(field=field):
                self.assert_rejected(field, 1, message)

    def test_requires_device_returned_grant_evidence(self):
        self.assert_rejected(
            "grant_evidence",
            "benchmark-exact-grant-check",
            "type2-hwcc requires device-returned exact grant evidence",
        )

    def test_rejects_missing_physical_backing_identity(self):
        row = valid_row()
        row["backing"] = {"allocation_count": 1, "registered": True}
        self.assertEqual(["type2-hwcc requires one physical backing identity"], validate_row(row))

    def test_rejects_wrong_backing_cardinality_or_size(self):
        row = valid_row()
        row["backing"]["allocation_count"] = 2
        self.assertIn("type2-hwcc requires one physical backing allocation", validate_row(row))
        row = valid_row()
        row["backing"]["mapped_size_bytes"] = 1048576
        self.assertIn("type2-hwcc backing sizes must match", validate_row(row))

    def test_rejects_wrong_endpoints_topk_and_correctness(self):
        self.assert_rejected("host_endpoint", 1, "type2-hwcc host endpoint must be 0")
        self.assert_rejected("device_endpoint", 0, "type2-hwcc device endpoint must be 1")
        self.assert_rejected("topk", 5, "topk must be 10")
        self.assert_rejected("correct", False, "type2-hwcc correctness must pass")

    def test_rejects_hwcc_counter_and_copy_failures(self):
        self.assertEqual(
            ["type2-hwcc requires post-update snoop ACKs"],
            validate_row(valid_row(snoop_acks=0)),
        )
        self.assertEqual(
            ["type2-hwcc copied_bytes must be zero"],
            validate_row(valid_row(copied_bytes=64)),
        )
        self.assert_rejected("gets", 0, "type2-hwcc requires GETS transitions")
        row = valid_row(getm=0, upgrade=0)
        self.assertIn("type2-hwcc requires GETM or UPGRADE transitions", validate_row(row))

    def test_zero_update_hwcc_requires_explicit_qualification_probe(self):
        row = valid_row(update_ratio=0.0)
        self.assertIn(
            "zero-update type2-hwcc requires an unmeasured coherence qualification probe",
            validate_row(row),
        )
        row["qualification_probe"] = True
        self.assertEqual([], validate_row(row))

    def test_rejects_wrong_copy_semantics(self):
        self.assertIn(
            "software-cc copied_bytes must equal dirty_lines * 64",
            validate_row(valid_row("software-cc", copied_bytes=63)),
        )
        self.assertIn(
            "full-copy copied_bytes must equal rows * dim * 4",
            validate_row(valid_row("full-copy", copied_bytes=64)),
        )

    def test_rejects_bad_native_and_negative_controls(self):
        self.assertIn(
            "native-gpu requires correct real cuda-driver execution",
            validate_row(valid_row("native-gpu", correct=False)),
        )
        self.assertIn(
            "negative-stale requires stale_observed=true and correct=false",
            validate_row(valid_row("negative-stale", correct=True)),
        )


class ValidateRunDirectoryTests(unittest.TestCase):
    @staticmethod
    def workload_from_row(row, epochs=1):
        return {
            "rows": row["rows"],
            "queries": row["queries"],
            "update_ratio": row["update_ratio"],
            "warmup": row["warmup"],
            "epochs": epochs,
            "seed": row["seed"],
            "dim": row["dim"],
            "topk": row["topk"],
        }

    def write_run(self, root, rows, manifest=None):
        root = Path(root)
        with (root / "results.jsonl").open("w", encoding="utf-8") as output:
            for row in rows:
                output.write(json.dumps(row) + "\n")
        write_summary(rows, root / "summary.csv")
        (root / "evidence.txt").write_text("fixture evidence\n", encoding="utf-8")
        first = rows[0]
        default_manifest = {
            "status": "pass",
            "budget_seconds": 28800,
            "row_count": len(rows),
            "commands": ["fixture command"],
            "commits": {
                "superproject": first["superproject_commit"],
                "qemu": first["qemu_commit"],
                "qemu_gitlink": first["qemu_gitlink_commit"],
            },
            "gpu_inventory": first["gpu_inventory"],
            "workloads": [self.workload_from_row(first)],
            "evidence_paths": ["evidence.txt"],
        }
        (root / "manifest.json").write_text(
            json.dumps(manifest or default_manifest), encoding="utf-8"
        )

    def test_rejects_malformed_json(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "results.jsonl").write_text("{broken\n", encoding="utf-8")
            (root / "manifest.json").write_text('{"status":"pass"}', encoding="utf-8")
            self.assertEqual(["results.jsonl:1: malformed JSON"], validate_run_dir(root))

    def test_rejects_missing_negative_stale_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            self.write_run(
                directory,
                [valid_row(mode) for mode in ("type2-hwcc", "software-cc", "full-copy", "native-gpu")],
            )
            self.assertIn("run requires negative-stale evidence", validate_run_dir(Path(directory)))

    def test_accepts_complete_validated_run(self):
        with tempfile.TemporaryDirectory() as directory:
            self.write_run(
                directory,
                [
                    valid_row(mode)
                    for mode in ("type2-hwcc", "software-cc", "full-copy", "native-gpu", "negative-stale")
                ],
            )
            self.assertEqual([], validate_run_dir(Path(directory)))

    def test_rejects_missing_summary_and_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            rows = [
                valid_row(mode)
                for mode in ("type2-hwcc", "software-cc", "full-copy", "native-gpu", "negative-stale")
            ]
            self.write_run(directory, rows)
            root = Path(directory)
            (root / "summary.csv").unlink()
            (root / "evidence.txt").unlink()
            errors = validate_run_dir(root)
            self.assertIn("summary.csv is missing or empty", errors)
            self.assertIn("manifest evidence path is missing: evidence.txt", errors)

    def test_requires_exactly_all_modes_for_each_manifest_workload(self):
        with tempfile.TemporaryDirectory() as directory:
            rows = [valid_row(mode) for mode in MODES_FOR_TEST]
            self.write_run(directory, rows[:-1])
            self.assertIn(
                "manifest workload 0 mode negative-stale has 0 rows; expected 1",
                validate_run_dir(Path(directory)),
            )

    def test_rejects_extra_workloads_and_modes(self):
        with tempfile.TemporaryDirectory() as directory:
            rows = [valid_row(mode) for mode in MODES_FOR_TEST]
            rows.append(valid_row("native-gpu", rows=8192, epoch=0))
            self.write_run(directory, rows)
            self.assertIn("results contain rows outside manifest workloads", validate_run_dir(Path(directory)))

    def test_requires_unique_contiguous_epoch_ids(self):
        with tempfile.TemporaryDirectory() as directory:
            rows = []
            for mode in MODES_FOR_TEST:
                rows.extend((valid_row(mode, epochs=2, epoch=0), valid_row(mode, epochs=2, epoch=1)))
            manifest = None
            self.write_run(directory, rows, manifest)
            root = Path(directory)
            manifest_data = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
            manifest_data["workloads"][0]["epochs"] = 2
            (root / "manifest.json").write_text(json.dumps(manifest_data), encoding="utf-8")
            self.assertEqual([], validate_run_dir(root))

            rows[-1]["epoch"] = 0
            self.write_run(root, rows, manifest_data)
            errors = validate_run_dir(root)
            self.assertIn(
                "manifest workload 0 mode negative-stale epoch IDs must be unique and contiguous 0..1",
                errors,
            )

    def test_rejects_malformed_epoch_id_without_crashing(self):
        with tempfile.TemporaryDirectory() as directory:
            rows = []
            for mode in MODES_FOR_TEST:
                rows.extend((valid_row(mode, epochs=2, epoch=0), valid_row(mode, epochs=2, epoch=1)))
            rows[0]["epoch"] = None
            self.write_run(directory, rows)
            root = Path(directory)
            manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
            manifest["workloads"][0]["epochs"] = 2
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            self.assertIn(
                "manifest workload 0 mode type2-hwcc epoch IDs must be unique and contiguous 0..1",
                validate_run_dir(root),
            )

    def test_rejects_missing_extra_and_altered_summary_rows(self):
        rows = [valid_row(mode) for mode in MODES_FOR_TEST]
        for mutation, expected in (
            ("missing", "summary.csv keys do not match results.jsonl groups"),
            ("extra", "summary.csv keys do not match results.jsonl groups"),
            ("altered-median", "summary.csv type2-hwcc end_to_end_ms_median does not match results.jsonl"),
            ("altered-p25", "summary.csv type2-hwcc end_to_end_ms_p25 does not match results.jsonl"),
            ("altered-p75", "summary.csv type2-hwcc end_to_end_ms_p75 does not match results.jsonl"),
        ):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                self.write_run(directory, rows)
                path = Path(directory) / "summary.csv"
                with path.open(newline="", encoding="utf-8") as source:
                    reader = csv.DictReader(source)
                    fieldnames = reader.fieldnames
                    records = list(reader)
                if mutation == "missing":
                    records.pop()
                elif mutation == "extra":
                    extra = dict(records[0])
                    extra["rows"] = "8192"
                    records.append(extra)
                else:
                    record = next(record for record in records if record["mode"] == "type2-hwcc")
                    suffix = mutation.removeprefix("altered-")
                    record[f"end_to_end_ms_{suffix}"] = "1.000001"
                with path.open("w", newline="", encoding="utf-8") as output:
                    writer = csv.DictWriter(output, fieldnames=fieldnames)
                    writer.writeheader()
                    writer.writerows(records)
                self.assertIn(expected, validate_run_dir(Path(directory)))


MODES_FOR_TEST = ("type2-hwcc", "software-cc", "full-copy", "native-gpu", "negative-stale")


class EnrichRowsTests(unittest.TestCase):
    def setUp(self):
        self.inventory = valid_row()["gpu_inventory"]
        self.commits = {"superproject": "a" * 40, "qemu": "b" * 40, "qemu_gitlink": "b" * 40}
        self.workload = Workload(rows=4096, queries=8, update_ratio=1.0 / 4096.0, warmup=0, epochs=1)

    def test_rejects_missing_raw_device_grant_evidence(self):
        raw = valid_row()
        for field in ("lines_requested", "lines_granted", "partial_grants", "grant_evidence"):
            raw.pop(field)
        with self.assertRaisesRegex(ValueError, "benchmark grant evidence is missing"):
            enrich_rows([raw], self.inventory, self.commits, {}, self.workload)

    def test_preserves_raw_device_grant_evidence(self):
        raw = valid_row(lines_requested=17, lines_granted=16, partial_grants=1)
        enriched = enrich_rows([raw], self.inventory, self.commits, {}, self.workload)
        self.assertEqual(
            (17, 16, 1, "device-returned-exact-grant"),
            tuple(
                enriched[0][field]
                for field in ("lines_requested", "lines_granted", "partial_grants", "grant_evidence")
            ),
        )


class DryRunTests(unittest.TestCase):
    def test_configured_path_honors_environment_override(self):
        default = Path("/default/base.img")
        with mock.patch.dict(os.environ, {"VECTORDB_BASE_IMAGE": "/mnt/disk0/isolated.img"}):
            self.assertEqual(
                Path("/mnt/disk0/isolated.img"),
                configured_path("VECTORDB_BASE_IMAGE", default),
            )

    def test_smoke_dry_run_prints_audited_configuration(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = main(["--dry-run", "--smoke"])
        self.assertEqual(0, status)
        text = output.getvalue()
        for token in (
            "type2-hwcc",
            "software-cc",
            "full-copy",
            "native-gpu",
            "negative-stale",
            "coherence-v2=on",
            "coherence-v2-host-endpoint=0",
            "coherence-v2-device-endpoint=1",
            "coherence-v2-cache-capacity=",
            "gpu-mode=2",
            "hetgpu-backend=3",
            "hetgpu-lib=/usr/lib/x86_64-linux-gnu/libcuda.so.1",
            "mem-size=512M",
            "cxl-fmw.0.size=512M",
            "-accel kvm",
            "-cpu host",
            "systemd.mask=cxl-numa-setup.service",
            "LD_LIBRARY_PATH=/root/vectordb",
            "resource2",
            "resource4",
            "0x8086",
            "0x0d92",
            "qemu-img create -f qcow2",
            "git rev-parse HEAD:lib/qemu",
            "lscpu",
            "uname -a",
            "systemctl poweroff --no-block",
        ):
            with self.subTest(token=token):
                self.assertIn(token, text)
        self.assertNotIn("setup_cxl_numa.sh", text)
        self.assertNotIn("gpu-mode=1", text)
        self.assertNotIn("backend=5", text)
        self.assertNotIn("simulation", text.lower())


class EvidenceParserTests(unittest.TestCase):
    SERVER_LOG = """
[info] Coherence v2 MESI Statistics:
[info]   Directory Lines: 32768
[info]   GETS: 32768
[info]   GETM: 8
[info]   UPGRADE: 0
[info]   PUTS: 0
[info]   PUTM: 0
[info]   Administrative Evict: 0
[info]   Timeout: 0
[info]   Partial ACK: 0
[info]   Stale ACK: 0
[info]   Invalid Ownership: 0
[info]   Snoop ACK: 8
"""

    QEMU_LOG = """
CXL Type2: Coherent pool initialized: base=0x10000000 size=512 MB (trapping overlay at priority 2)
CXL Type2: Coherent pool GPU mapping: host=0x7f0000000000 device=0x7f1000000000 size=536870912
CXL Type2: protocol-v2 host endpoint 0 session 0x11, device endpoint 1 session 0x22, policy=write-back
"""

    def test_parses_final_server_counters(self):
        evidence = parse_server_evidence(self.SERVER_LOG)
        self.assertEqual(32768, evidence["gets"])
        self.assertEqual(8, evidence["snoop_acks"])
        self.assertEqual(0, evidence["partial_ack_events"])

    def test_server_parser_fails_closed_on_missing_counter(self):
        with self.assertRaisesRegex(ValueError, "missing final server counters: snoop_acks"):
            parse_server_evidence(self.SERVER_LOG.replace("[info]   Snoop ACK: 8\n", ""))

    def test_parses_single_physical_qemu_backing(self):
        evidence = parse_qemu_evidence(self.QEMU_LOG, 128 * 1024 * 1024)
        self.assertEqual(0, evidence["host_endpoint"])
        self.assertEqual(1, evidence["device_endpoint"])
        self.assertEqual(536870912, evidence["backing"]["size_bytes"])
        self.assertEqual("0x7f0000000000", evidence["backing"]["host_identity"])

    def test_qemu_parser_rejects_partial_or_failed_path(self):
        with self.assertRaisesRegex(ValueError, "QEMU reported coherence/GPU errors"):
            parse_qemu_evidence(self.QEMU_LOG + "CXL Type2: partial range grant failed\n", 4096)

    def test_qemu_parser_rejects_ambiguous_backing(self):
        with self.assertRaisesRegex(ValueError, "identity is missing or ambiguous"):
            parse_qemu_evidence(self.QEMU_LOG + self.QEMU_LOG, 4096)

    def test_qemu_parser_rejects_old_policy_format_wrong_policy_and_fallbacks(self):
        old = self.QEMU_LOG.replace(", policy=write-back", " (write-back)")
        mixed = self.QEMU_LOG + old.splitlines()[-1] + "\n"
        wrong = self.QEMU_LOG.replace("policy=write-back", "policy=write-through")
        fallback = self.QEMU_LOG + "CXL hetGPU: falling back to simulation backend\n"
        simulation = self.QEMU_LOG + "CXL Type2: simulation mode selected\n"
        with self.assertRaisesRegex(ValueError, "legacy protocol-v2 evidence"):
            parse_qemu_evidence(old, 4096)
        with self.assertRaisesRegex(ValueError, "legacy protocol-v2 evidence"):
            parse_qemu_evidence(mixed, 4096)
        with self.assertRaisesRegex(ValueError, "policy is not write-back"):
            parse_qemu_evidence(wrong, 4096)
        with self.assertRaisesRegex(ValueError, "coherence/GPU errors"):
            parse_qemu_evidence(fallback, 4096)
        with self.assertRaisesRegex(ValueError, "coherence/GPU errors"):
            parse_qemu_evidence(simulation, 4096)

    def test_benchmark_parser_rejects_non_json_stdout(self):
        with self.assertRaisesRegex(ValueError, "line 2 is not JSON"):
            parse_benchmark_jsonl('{"mode":"native-gpu"}\nwarning\n')


class ProcessAndCommandTests(unittest.TestCase):
    def test_stop_process_escalates_after_bounded_wait(self):
        process = mock.Mock(pid=4321)
        process.poll.return_value = None
        process.wait.side_effect = [TimeoutExpired("qemu", 1), 0]
        with mock.patch("script.run_type2_vectordb.os.killpg") as killpg:
            stop_process(process, timeout=1)
        self.assertEqual(
            [mock.call(4321, sent_signal) for sent_signal in (signal.SIGTERM, signal.SIGKILL)],
            killpg.call_args_list,
        )
        self.assertEqual([mock.call(timeout=1), mock.call(timeout=1)], process.wait.call_args_list)

    def test_qemu_command_has_real_type2_and_bounded_cache(self):
        command = " ".join(qemu_command(Path("overlay.qcow2"), 19000, 20000, 128 * 1024 * 1024))
        for token in (
            "gpu-mode=2",
            "hetgpu-backend=3",
            "mem-size=512M",
            "coherence-v2=on",
            "coherence-v2-host-endpoint=0",
            "coherence-v2-device-endpoint=1",
            "coherence-v2-cache-capacity=134217728",
            "coherence-v2-write-through=off",
            "cxl-fmw.0.size=512M",
        ):
            self.assertIn(token, command)


if __name__ == "__main__":
    unittest.main()
