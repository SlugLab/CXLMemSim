import csv
import json
import tempfile
import unittest
from pathlib import Path

from script.plot_cxlmem_hwcc_validation import PlotValidationError, generate_outputs


FIELDS = ("backend", "mode", "placement", "repetition", "event", "value", "operations")


class CxlmemHwccPlotTest(unittest.TestCase):
    def write_run(self, root: Path, status: str = "pass") -> None:
        (root / "validation.json").write_text(
            json.dumps(
                {
                    "schema": "splash.cxlmem-hwcc-validation.v1",
                    "status": status,
                    "proof_gates": {"litmus": "pass"},
                }
            ),
            encoding="utf-8",
        )
        (root / "summary.json").write_text(
            json.dumps({"litmus": {"stale": 0}, "atomic": {"ticket_errors": 0, "cas_errors": 0}}),
            encoding="utf-8",
        )
        rows = []
        values = (10.0, 20.0, 30.0, 40.0, 50.0)
        for repetition, value in enumerate(values):
            rows.extend(
                (
                    ("dram", "warm-load", "same-numa", repetition, "benchmark.average_ns", value, 100),
                    ("cxlmem", "cold-load", "same-numa", repetition, "benchmark.average_ns", value * 2, 100),
                    (
                        "cxlmem",
                        "cold-load",
                        "same-numa",
                        repetition,
                        "mem_load_retired.local_cxl_mem",
                        value,
                        100,
                    ),
                    (
                        "cxlmem",
                        "cold-load",
                        "same-numa",
                        repetition,
                        "cxl_pmu_mem0.0/m2s_req_memrd/",
                        value * 2,
                        100,
                    ),
                    (
                        "cxlmem",
                        "handoff",
                        "cross-numa",
                        repetition,
                        "mem_load_l3_hit_retired.xsnp_fwd",
                        value * 3,
                        100,
                    ),
                )
            )
        with (root / "results.csv").open("w", newline="", encoding="utf-8") as output:
            writer = csv.writer(output)
            writer.writerow(FIELDS)
            writer.writerows(rows)

    def test_rejects_nonpassing_validation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_run(root, "fail")
            with self.assertRaisesRegex(PlotValidationError, "validated passing run"):
                generate_outputs(root, root / "paper")

    def test_writes_deterministic_aggregates_and_figure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_run(root)
            first = generate_outputs(root, root / "paper-a")
            second = generate_outputs(root, root / "paper-b")

            self.assertEqual(first.csv_path.read_bytes(), second.csv_path.read_bytes())
            self.assertEqual(first.pdf_path.read_bytes(), second.pdf_path.read_bytes())
            self.assertTrue(first.pdf_path.read_bytes().startswith(b"%PDF"))
            with first.csv_path.open(newline="", encoding="utf-8") as source:
                records = list(csv.DictReader(source))
            dram = next(row for row in records if row["series"] == "DRAM warm load")
            cpu_pmu = next(row for row in records if row["series"] == "CXL cold CPU CXL-read")
            self.assertEqual(dram["median"], "30")
            self.assertEqual(dram["p25"], "20")
            self.assertEqual(dram["p75"], "40")
            self.assertEqual(cpu_pmu["median"], "0.3")
            self.assertEqual(cpu_pmu["unit"], "events/op")
            self.assertTrue(all(row["stale_reads"] == "0" for row in records))
            self.assertTrue(all(row["atomic_errors"] == "0" for row in records))


if __name__ == "__main__":
    unittest.main()
