import contextlib
import io
import json
import signal
import tempfile
import unittest
from pathlib import Path
from subprocess import TimeoutExpired
from unittest import mock

from script.run_type2_vectordb import (
    main,
    parse_benchmark_jsonl,
    parse_qemu_evidence,
    parse_server_evidence,
    qemu_command,
    stop_process,
    validate_row,
    validate_run_dir,
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
        "grant_evidence": "benchmark-exact-grant-check",
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
    def write_run(self, root, rows, manifest=None):
        root = Path(root)
        with (root / "results.jsonl").open("w", encoding="utf-8") as output:
            for row in rows:
                output.write(json.dumps(row) + "\n")
        (root / "summary.csv").write_text("mode,epochs\ntype2-hwcc,1\n", encoding="utf-8")
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


class DryRunTests(unittest.TestCase):
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
            "qemu-img create -f qcow2",
            "git rev-parse HEAD:lib/qemu",
            "lscpu",
            "uname -a",
            "systemctl poweroff --no-block",
        ):
            with self.subTest(token=token):
                self.assertIn(token, text)
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
CXL Type2: Coherent pool initialized: base=0x10000000 size=256 MB (trapping overlay at priority 2)
CXL Type2: Coherent pool GPU mapping: host=0x7f0000000000 device=0x7f1000000000 size=268435456
CXL Type2: protocol-v2 host endpoint 0 session 0x11, device endpoint 1 session 0x22 (write-back)
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
        self.assertEqual(268435456, evidence["backing"]["size_bytes"])
        self.assertEqual("0x7f0000000000", evidence["backing"]["host_identity"])

    def test_qemu_parser_rejects_partial_or_failed_path(self):
        with self.assertRaisesRegex(ValueError, "QEMU reported coherence/GPU errors"):
            parse_qemu_evidence(self.QEMU_LOG + "CXL Type2: partial range grant failed\n", 4096)

    def test_qemu_parser_rejects_ambiguous_backing(self):
        with self.assertRaisesRegex(ValueError, "identity is missing or ambiguous"):
            parse_qemu_evidence(self.QEMU_LOG + self.QEMU_LOG, 4096)

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
