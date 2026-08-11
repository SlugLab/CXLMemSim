import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from script.plot_type2_vectordb import (
    UPDATE_MODES,
    PlotValidationError,
    _use_log_scale,
    generate_paper_subset_plots,
)
from script.run_type2_vectordb import MODES, Workload, write_summary


PAPER_POINTS = (
    (16384, 16, 0.001),
    (65536, 1, 0.001),
    (65536, 16, 0.0),
    (65536, 16, 0.0001),
    (65536, 16, 0.001),
    (65536, 16, 0.01),
    (65536, 64, 0.001),
    (262144, 16, 0.001),
)


def make_row(mode, workload, epoch):
    mode_index = MODES.index(mode)
    dirty_lines = 0 if workload.update_ratio == 0 else max(1, int(workload.rows * workload.update_ratio))
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
        "rows": workload.rows,
        "dim": workload.dim,
        "queries": workload.queries,
        "topk": workload.topk,
        "update_ratio": workload.update_ratio,
        "warmup": workload.warmup,
        "epochs": workload.epochs,
        "epoch": epoch,
        "seed": workload.seed,
        "copied_bytes": 0,
        "dirty_lines": dirty_lines,
        "correct": True,
        "stale_observed": False,
        "protocol_version": 2,
        "host_endpoint": 0,
        "device_endpoint": 1,
        "server_commit": "a" * 40,
        "superproject_commit": "a" * 40,
        "qemu_commit": "b" * 40,
        "qemu_gitlink_commit": "b" * 40,
        "lines_requested": workload.rows * workload.dim * 4 // 64,
        "lines_granted": workload.rows * workload.dim * 4 // 64,
        "partial_grants": 0,
        "grant_evidence": "device-returned-exact-grant",
        "end_to_end_ms": 10.0 + mode_index + epoch / 10.0,
        "update_ms": 1.0 + mode_index / 10.0,
        "synchronization_ms": 2.0 + mode_index / 10.0 + epoch / 100.0,
        "kernel_ms": 5.0 + mode_index / 10.0,
        "qps": workload.queries * 1000.0 / (10.0 + mode_index + epoch / 10.0),
        "p50_query_ms": 0.5 + mode_index / 10.0,
        "p99_query_ms": 0.8 + mode_index / 10.0,
        "gets": workload.rows,
        "getm": max(1, dirty_lines),
        "upgrade": 0,
        "puts": 0,
        "putm": 0,
        "snoop_acks": max(1, dirty_lines),
        "directory_lines": workload.rows,
        "directory_evictions": 0,
        "timeout_events": 0,
        "partial_ack_events": 0,
        "stale_ack_events": 0,
        "invalid_ownership_events": 0,
        "backing": {
            "allocation_count": 1,
            "host_identity": "0x7f0000000000",
            "device_identity": "0x7f0000000000",
            "size_bytes": workload.rows * workload.dim * 4,
            "mapped_size_bytes": workload.rows * workload.dim * 4,
            "registered": True,
        },
    }
    if mode == "type2-hwcc" and workload.update_ratio == 0:
        row["qualification_probe"] = True
    elif mode == "software-cc":
        row["copied_bytes"] = dirty_lines * 64
    elif mode == "full-copy":
        row["copied_bytes"] = workload.rows * workload.dim * 4
    elif mode == "negative-stale":
        row.update(correct=False, stale_observed=True, observed_label=0, expected_label=1410)
    return row


def write_valid_run(root):
    workloads = [Workload(rows, queries, ratio, warmup=5, epochs=10) for rows, queries, ratio in PAPER_POINTS]
    rows = [make_row(mode, workload, epoch) for workload in workloads for mode in MODES for epoch in range(10)]
    with (root / "results.jsonl").open("w", encoding="utf-8") as output:
        for row in rows:
            output.write(json.dumps(row, sort_keys=True) + "\n")
    write_summary(rows, root / "summary.csv")
    (root / "evidence.txt").write_text("validated fixture evidence\n", encoding="utf-8")
    manifest = {
        "schema": "splash.vectordb.v1",
        "status": "pass",
        "budget_seconds": 28800,
        "row_count": len(rows),
        "commands": [
            "cmake --build build --target cxlmemsim_server -j",
            "ssh root@127.0.0.1 'vectordb_shared_index --mode type2-hwcc --rows 16384'",
        ],
        "commits": {
            "superproject": "a" * 40,
            "qemu": "b" * 40,
            "qemu_gitlink": "b" * 40,
        },
        "gpu_inventory": rows[0]["gpu_inventory"],
        "inputs": {
            "base_image": "/images/qemu-node0.img",
            "kernel_image": "/images/bzImage",
            "qemu_binary": "/build/qemu-system-x86_64",
            "real_libcuda": "/usr/lib/x86_64-linux-gnu/libcuda.so.1",
            "server_binary": "/build/cxlmemsim_server",
        },
        "workloads": [workload.__dict__ for workload in workloads],
        "evidence_paths": ["evidence.txt"],
        "started_utc": "2026-08-11T00:00:00+00:00",
        "completed_utc": "2026-08-11T01:02:03+00:00",
    }
    (root / "manifest.json").write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")


class PlotType2VectorDbTests(unittest.TestCase):
    def test_update_panels_exclude_compute_only_native_mode(self):
        self.assertEqual(("type2-hwcc", "software-cc", "full-copy"), UPDATE_MODES)

    def test_script_entrypoint_is_runnable_from_repository_root(self):
        result = subprocess.run(
            [sys.executable, "script/plot_type2_vectordb.py", "--help"],
            cwd=Path(__file__).resolve().parents[1],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("validated run directory or its summary.csv", result.stdout)

    def test_uses_log_scale_only_for_material_metric_range(self):
        self.assertFalse(_use_log_scale([1200.0, 1500.0]))
        self.assertTrue(_use_log_scale([100.0, 2001.0]))

    def test_generates_provenance_csv_and_three_panel_pdf_png(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_valid_run(root)

            outputs = generate_paper_subset_plots(root)

            self.assertEqual(
                {
                    root / "figures" / "type2_vectordb_paper_subset.csv",
                    root / "figures" / "type2_vectordb_performance.pdf",
                    root / "figures" / "type2_vectordb_performance.png",
                    root / "figures" / "type2_vectordb_update_cost.pdf",
                    root / "figures" / "type2_vectordb_update_cost.png",
                },
                set(outputs),
            )
            for output in outputs[1:]:
                expected = b"%PDF" if output.suffix == ".pdf" else b"\x89PNG\r\n\x1a\n"
                self.assertTrue(output.read_bytes().startswith(expected))
            with outputs[0].open(newline="", encoding="utf-8") as source:
                records = list(csv.DictReader(source))
            self.assertEqual(32, len(records))
            self.assertEqual(
                {"type2-hwcc", "software-cc", "full-copy", "native-gpu"},
                {record["mode"] for record in records},
            )
            self.assertEqual({root.name}, {record["source_run_id"] for record in records})
            self.assertEqual({"a" * 40}, {record["superproject_commit"] for record in records})
            self.assertEqual({"b" * 40}, {record["qemu_commit"] for record in records})
            self.assertTrue(all(len(record["manifest_sha256"]) == 64 for record in records))
            self.assertTrue(all(len(record["results_sha256"]) == 64 for record in records))
            self.assertTrue(all(len(record["summary_sha256"]) == 64 for record in records))
            self.assertTrue(all(len(record["plotter_sha256"]) == 64 for record in records))
            self.assertEqual({"2026-08-11T00:00:00+00:00"}, {record["started_utc"] for record in records})
            self.assertEqual({"/images/qemu-node0.img"}, {record["base_image"] for record in records})
            self.assertEqual(
                {"ssh root@127.0.0.1 'vectordb_shared_index --mode type2-hwcc --rows 16384'"},
                {record["experiment_command"] for record in records},
            )

    def test_cli_accepts_summary_csv_and_generates_all_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_valid_run(root)
            output_dir = root / "paper-figures"

            result = subprocess.run(
                [
                    sys.executable,
                    "script/plot_type2_vectordb.py",
                    str(root / "summary.csv"),
                    "--output-dir",
                    str(output_dir),
                ],
                cwd=Path(__file__).resolve().parents[1],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual(5, len(list(output_dir.iterdir())))

    def assert_fails_without_outputs(self, mutate, expected):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write_valid_run(root)
            mutate(root)
            with self.assertRaisesRegex(PlotValidationError, expected):
                generate_paper_subset_plots(root)
            self.assertFalse((root / "figures").exists())

    def test_rejects_any_missing_paper_subset_workload(self):
        def mutate(root):
            manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
            manifest["workloads"].pop()
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

        self.assert_fails_without_outputs(mutate, "exactly the 8 paper-subset workloads")

    def test_rejects_negative_stale_that_does_not_demonstrate_staleness(self):
        def mutate(root):
            rows = [json.loads(line) for line in (root / "results.jsonl").read_text(encoding="utf-8").splitlines()]
            negative = next(row for row in rows if row["mode"] == "negative-stale")
            negative.update(correct=True, stale_observed=False)
            (root / "results.jsonl").write_text(
                "".join(json.dumps(row, sort_keys=True) + "\n" for row in rows), encoding="utf-8"
            )

        self.assert_fails_without_outputs(mutate, "negative-stale requires stale_observed=true and correct=false")

    def test_rejects_summary_not_matching_validated_results(self):
        def mutate(root):
            path = root / "summary.csv"
            with path.open(newline="", encoding="utf-8") as source:
                reader = csv.DictReader(source)
                fieldnames = reader.fieldnames
                records = list(reader)
            records[0]["qps_median"] = "999999"
            with path.open("w", newline="", encoding="utf-8") as output:
                writer = csv.DictWriter(output, fieldnames=fieldnames)
                writer.writeheader()
                writer.writerows(records)

        self.assert_fails_without_outputs(mutate, "summary.csv .* qps_median does not match results.jsonl")

    def test_rejects_incomplete_provenance(self):
        def mutate(root):
            manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
            del manifest["completed_utc"]
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

        self.assert_fails_without_outputs(mutate, "completed_utc")


if __name__ == "__main__":
    unittest.main()
