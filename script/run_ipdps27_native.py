#!/usr/bin/env python3
"""Re-run the archived VectorDB GPU oracle on the local GPU, without CXL.

The immutable source revision is deliberately separate from the current
Type-2 checkout. Native results are a portability/correctness check, not
substitutes for a new Type-2 full-system run.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
REVISION = "4869bb699dfcbefe6c6495281da8d513ad9a3d56"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--nvcc", default="/usr/local/cuda/bin/nvcc")
    parser.add_argument("--host-compiler", default="g++")
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    sources = args.out / "sources"
    sources.mkdir()
    commands = []
    manifest = {"schema": "splash.ipdps27.native-oracle.v1", "source_commit": REVISION,
                "platform": platform.platform(), "commands": commands,
                "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "scope": "native CUDA only; no Type-2 device or CXL timing", "status": "running"}

    def save_manifest():
        (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    def run(cmd, stem, timeout=180):
        commands.append(cmd)
        save_manifest()
        with (args.out / f"{stem}.stdout").open("w") as out, (args.out / f"{stem}.stderr").open("w") as err:
            subprocess.run(cmd, cwd=sources, stdout=out, stderr=err, check=True, timeout=timeout)

    try:
        for name in ("vectordb_shared_index.c", "vectordb_shared_index_kernel.cu"):
            content = subprocess.check_output(["git", "-C", str(ROOT), "show", f"{REVISION}:qemu_integration/guest_libcuda/{name}"])
            (sources / name).write_bytes(content)
        manifest["source_hashes"] = {p.name: digest(p) for p in sources.iterdir()}
        manifest["gpu_inventory"] = subprocess.check_output([
            "nvidia-smi", "--query-gpu=name,uuid,driver_version,memory.total,memory.used,utilization.gpu", "--format=csv,noheader"], text=True).strip()
        manifest["compiler"] = subprocess.check_output([args.nvcc, "--version"], text=True)
        run([args.nvcc, "-ccbin", args.host_compiler, "-Xcompiler=-U_GNU_SOURCE", "-O3", "--ptx",
             "--gpu-architecture=compute_80", "--fmad=false", "-o", "kernel.ptx", "vectordb_shared_index_kernel.cu"], "compile-ptx")
        run(["xxd", "-i", "-n", "kVectordbSharedIndexPtx", "kernel.ptx", "vectordb_shared_index_kernel_ptx.h"], "embed-ptx")
        binary = args.out / "vectordb_native"
        run(["gcc", "-O2", "-Wall", "-Wextra", "-ffp-contract=off", "-DVECTORDB_NATIVE_BUILD=1",
             "-o", str(binary), "vectordb_shared_index.c", "-lcuda", "-ldl", "-lm"], "compile-native")
        manifest["binary_sha256"] = digest(binary)
        original = json.loads((ROOT / "697a667df59bdd0085f8ce88/artifacts/type2_vectordb_20260811T004358Z/manifest.json").read_text())
        manifest["workloads"] = original["workloads"]
        all_rows = []
        for index, workload in enumerate(manifest["workloads"]):
            for mode in ("native-gpu", "negative-stale"):
                cmd = [str(binary), "--mode", mode]
                for key in ("rows", "dim", "queries", "topk", "update_ratio", "warmup", "epochs", "seed"):
                    cmd += ["--" + key.replace("_", "-"), str(workload[key])]
                stem = f"point-{index:02d}-{mode}"
                run(cmd, stem)
                rows = [json.loads(line) for line in (args.out / f"{stem}.stdout").read_text().splitlines() if line.startswith("{")]
                if len(rows) != workload["epochs"] or [r["epoch"] for r in rows] != list(range(workload["epochs"])):
                    raise RuntimeError(f"{stem}: incomplete or duplicated epoch set")
                for row in rows:
                    if row.get("real_gpu") is not True:
                        raise RuntimeError(f"{stem}: no physical GPU evidence")
                    if mode == "native-gpu" and (not row["correct"] or row["cpu_labels"] != row["gpu_labels"]):
                        raise RuntimeError(f"{stem}: exact top-k mismatch")
                    if mode == "negative-stale" and (row["correct"] or not row["stale_observed"]):
                        raise RuntimeError(f"{stem}: negative control did not detect staleness")
                all_rows.extend(rows)
                (args.out / "results.jsonl").write_text("".join(json.dumps(r, sort_keys=True) + "\n" for r in all_rows))
                print(f"{stem}: {len(rows)} epochs verified", flush=True)
        manifest["epochs_verified"] = len(all_rows)
        manifest["status"] = "pass"
    except Exception as error:
        manifest["status"] = "fail"
        manifest["error"] = str(error)
        raise
    finally:
        save_manifest()
        checksums = [f"{digest(p)}  {p.relative_to(args.out)}" for p in sorted(args.out.rglob("*")) if p.is_file() and p.name != "SHA256SUMS"]
        (args.out / "SHA256SUMS").write_text("\n".join(checksums) + "\n")


if __name__ == "__main__":
    main()
