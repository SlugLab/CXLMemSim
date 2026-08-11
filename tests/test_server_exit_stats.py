#!/usr/bin/env python3

import argparse
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import tempfile
import time
import unittest


SERVER_BIN = None
CLIENT_BIN = None


def wait_for_text(path: Path, needle: str, process: subprocess.Popen, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = path.read_text(errors="replace") if path.exists() else ""
        if needle in text:
            return text
        if process.poll() is not None:
            raise AssertionError(f"server exited before readiness (rc={process.returncode}):\n{text}")
        time.sleep(0.05)
    text = path.read_text(errors="replace") if path.exists() else ""
    raise AssertionError(f"timed out waiting for {needle!r}:\n{text}")


class ServerExitStatsTest(unittest.TestCase):
    def test_sigterm_emits_one_complete_snapshot(self):
        self.assertIsNotNone(SERVER_BIN)
        self.assertIsNotNone(CLIENT_BIN)

        token = f"{os.getpid()}_{secrets.token_hex(6)}"
        shm_name = f"/cxlmemsim_pgas_stats_{token}"
        shm_path = Path("/dev/shm") / shm_name.removeprefix("/")

        with tempfile.TemporaryDirectory(prefix="cxlmemsim-exit-stats-") as temporary:
            temporary_path = Path(temporary)
            backing_file = temporary_path / "backing.bin"
            stats_json = temporary_path / "stats.json"
            log_path = temporary_path / "server.log"
            process = None

            try:
                with log_path.open("w") as log:
                    process = subprocess.Popen(
                        [
                            SERVER_BIN,
                            "--comm-mode=pgas-shm",
                            f"--pgas-shm-name={shm_name}",
                            "--capacity=16",
                            "--backing-mode=file",
                            f"--backing-file={backing_file}",
                            f"--stats-json={stats_json}",
                        ],
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        text=True,
                    )

                    wait_for_text(log_path, "PGAS shared memory initialized", process, 10)
                    subprocess.run([CLIENT_BIN, shm_name], check=True, timeout=10)
                    process.send_signal(signal.SIGTERM)
                    self.assertEqual(process.wait(timeout=10), 0)

                output = log_path.read_text(errors="replace")
                self.assertEqual(output.count("Final Server Statistics:"), 1, output)
                self.assertTrue(stats_json.is_file(), output)
                stats = json.loads(stats_json.read_text())

                self.assertEqual(stats["schema"], "cxlmemsim.server-stats.v1")
                self.assertEqual(stats["communication_mode"], "pgas-shm")
                self.assertEqual(
                    stats["server"],
                    {
                        "reads": 1,
                        "writes": 1,
                        "atomic_faa": 1,
                        "atomic_cas": 1,
                        "atomic_cas_success": 1,
                        "fences": 1,
                    },
                )
                self.assertGreaterEqual(stats["controller"]["remote"], 4)
                self.assertGreaterEqual(stats["controller"]["threads_created"], 1)
                self.assertTrue(stats["switches"])
                self.assertGreater(sum(item["loads"] for item in stats["switches"]), 0)
                self.assertGreater(sum(item["stores"] for item in stats["switches"]), 0)
                self.assertTrue(stats["endpoints"])
                self.assertGreater(sum(item["loads"] for item in stats["endpoints"]), 0)
                self.assertGreater(sum(item["stores"] for item in stats["endpoints"]), 0)
            finally:
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                if shm_path.name.startswith("cxlmemsim_pgas_stats_"):
                    try:
                        shm_path.unlink()
                    except FileNotFoundError:
                        pass


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--client", required=True)
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    SERVER_BIN = args.server
    CLIENT_BIN = args.client
    unittest.main(argv=[__file__], verbosity=2)
