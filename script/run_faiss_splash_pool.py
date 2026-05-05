#!/usr/bin/env python3
import argparse
import os
import signal
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPLASH = (ROOT / "../Splash").resolve()
FAISS_SRC = ROOT / "workloads/faiss"
FAISS_BUILD = ROOT / "build/faiss_cpu"
BENCH_SRC = ROOT / "tools/faiss_splash/faiss_cpu_splash.cpp"
BENCH_BIN = FAISS_BUILD / "faiss_cpu_splash"
ARTIFACT = ROOT / "artifact/faiss_splash"


def run(cmd, **kwargs):
    print("+", " ".join(str(x) for x in cmd), flush=True)
    subprocess.run(cmd, check=True, **kwargs)


def configure_and_build_faiss(jobs):
    FAISS_BUILD.mkdir(parents=True, exist_ok=True)
    cache = FAISS_BUILD / "CMakeCache.txt"
    if not cache.exists():
        run([
            "cmake", "-S", str(FAISS_SRC), "-B", str(FAISS_BUILD),
            "-DCMAKE_BUILD_TYPE=Release",
            "-DFAISS_ENABLE_GPU=OFF",
            "-DFAISS_ENABLE_PYTHON=OFF",
            "-DFAISS_ENABLE_TESTS=OFF",
            "-DBUILD_TESTING=OFF",
            "-DFAISS_OPT_LEVEL=generic",
            "-DBLA_VENDOR=OpenBLAS",
        ])
    run(["cmake", "--build", str(FAISS_BUILD), "--target", "faiss", "-j", str(jobs)])


def find_one(patterns):
    for pattern in patterns:
        matches = sorted(FAISS_BUILD.glob(pattern))
        if matches:
            return matches[0]
    raise FileNotFoundError(f"no file matched {patterns}")


def build_benchmark():
    libfaiss = find_one(["faiss/libfaiss.a", "faiss/libfaiss.so", "**/libfaiss.a", "**/libfaiss.so"])
    splash_backend = SPLASH / "build/libcxl_backend.a"
    if not splash_backend.exists():
        raise FileNotFoundError(f"missing {splash_backend}; build Splash first")
    run([
        "g++", "-std=c++17", "-O3", "-fopenmp",
        "-I", str(FAISS_SRC),
        "-I", str(FAISS_BUILD),
        "-I", str(SPLASH / "src/libpgas/include"),
        str(BENCH_SRC),
        str(libfaiss),
        str(splash_backend),
        "-lopenblas", "-lpthread", "-lrt", "-ldl",
        "-o", str(BENCH_BIN),
    ])


def start_pool(args, log_path):
    log = open(log_path, "w", encoding="utf-8")
    if args.pool_provider == "splash":
        server = SPLASH / "build/cxl_shmem_server"
        if not server.exists():
            log.close()
            raise FileNotFoundError(f"missing {server}; build Splash first")
        cmd = [
            str(server),
            "--name", args.pool_name,
            "--size", str(args.capacity_mb * 1024 * 1024),
            "--latency", str(args.latency_ns),
        ]
        cwd = SPLASH
    else:
        server = ROOT / "build/cxlmemsim_server"
        if not server.exists():
            run(["cmake", "--build", str(ROOT / "build"), "--target", "cxlmemsim_server", "-j", "4"])
        cmd = [
            str(server),
            "--comm-mode", "pgas-shm",
            "--pgas-shm-name", args.pool_name,
            "--capacity", str(args.capacity_mb),
            "--default_latency", str(args.latency_ns),
        ]
        cwd = ROOT
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, cwd=cwd)
    time.sleep(2.0)
    if proc.poll() is not None:
        log.close()
        raise RuntimeError(f"cxlmemsim_server exited early, see {log_path}")
    return proc, log


def stop_proc(proc, log):
    if proc and proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.terminate()
            proc.wait(timeout=5)
    if log:
        log.close()


def bench_cmd(args, storage, node):
    return [
        str(BENCH_BIN),
        "--storage", storage,
        "--pool", args.pool_name,
        "--node", str(node),
        "--nb", str(args.nb),
        "--nq", str(args.nq),
        "--dim", str(args.dim),
        "--k", str(args.k),
        "--iters", str(args.iters),
        "--block", str(args.block),
        "--pool-mb", str(args.capacity_mb),
        "--threads", str(args.threads),
    ]


def run_node(args, storage, node, out_dir):
    env = os.environ.copy()
    env["PGAS_LOCAL_NODE"] = str(node)
    env["PGAS_NUM_NODES"] = "2"
    log_path = out_dir / f"{storage}_node{node}.log"
    with open(log_path, "w", encoding="utf-8") as log:
        proc = subprocess.run(bench_cmd(args, storage, node), stdout=log, stderr=subprocess.STDOUT, env=env, cwd=ROOT)
    if proc.returncode != 0:
        raise RuntimeError(f"{storage} node {node} failed, see {log_path}")
    return log_path


def parse_results(log_paths, csv_path):
    rows = []
    for log_path in log_paths:
        text = log_path.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            if not line.startswith("FAISS_SPLASH_RESULT"):
                continue
            row = {}
            for part in line.split()[1:]:
                key, value = part.split("=", 1)
                row[key] = value
            rows.append(row)
    fields = ["node", "storage", "nb", "nq", "dim", "k", "iters", "block", "db_mb",
              "total_ms", "pool_write_ms", "pool_read_ms", "qps", "checksum"]
    with open(csv_path, "w", encoding="utf-8") as f:
        f.write(",".join(fields) + "\n")
        for row in rows:
            f.write(",".join(row.get(field, "") for field in fields) + "\n")
    return rows


def main():
    parser = argparse.ArgumentParser(description="Run FAISS CPU against native DRAM and Splash/CXLMemSim SHMEM pool.")
    parser.add_argument("--nb", type=int, default=10000)
    parser.add_argument("--nq", type=int, default=64)
    parser.add_argument("--dim", type=int, default=64)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--iters", type=int, default=1)
    parser.add_argument("--block", type=int, default=2048)
    parser.add_argument("--capacity-mb", type=int, default=256)
    parser.add_argument("--latency-ns", type=int, default=100)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--pool-name", default="/faiss_cxl_pool")
    parser.add_argument("--pool-provider", choices=["splash", "cxlmemsim"], default="splash")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()

    if not FAISS_SRC.exists():
        raise FileNotFoundError(f"missing FAISS checkout at {FAISS_SRC}")
    if not SPLASH.exists():
        raise FileNotFoundError(f"missing Splash checkout at {SPLASH}")

    ARTIFACT.mkdir(parents=True, exist_ok=True)
    out_dir = ARTIFACT / time.strftime("run_%Y%m%d_%H%M%S")
    out_dir.mkdir()

    if not args.skip_build:
        configure_and_build_faiss(args.jobs)
        build_benchmark()

    logs = []
    for node in (0, 1):
        logs.append(run_node(args, "native", node, out_dir))

    proc = None
    pool_log = None
    try:
        proc, pool_log = start_pool(args, out_dir / f"{args.pool_provider}_pool.log")
        for node in (0, 1):
            logs.append(run_node(args, "cxl-pool", node, out_dir))
    finally:
        stop_proc(proc, pool_log)

    rows = parse_results(logs, out_dir / "results.csv")
    latest = ARTIFACT / "latest"
    if latest.exists() or latest.is_symlink():
        latest.unlink()
    latest.symlink_to(out_dir, target_is_directory=True)

    print(f"wrote {out_dir / 'results.csv'}")
    for row in rows:
        print(
            f"node={row['node']} storage={row['storage']} "
            f"total_ms={row['total_ms']} pool_read_ms={row['pool_read_ms']} qps={row['qps']}"
        )


if __name__ == "__main__":
    main()
