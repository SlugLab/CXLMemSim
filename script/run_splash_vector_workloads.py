#!/usr/bin/env python3
import argparse
import os
import shlex
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ANN_ROOT = ROOT / "workloads/ann-benchmarks"
FAISS_RUNNER = ROOT / "script/run_faiss_splash_pool.py"


def resolve_splash_root(explicit=None):
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    if os.environ.get("SPLASH_ROOT"):
        candidates.append(Path(os.environ["SPLASH_ROOT"]))
    candidates.extend([ROOT / "../splash", ROOT / "../Splash"])

    for candidate in candidates:
        root = candidate.expanduser().resolve()
        if (root / "src/libpgas/include/pgas/pgas.h").exists():
            return root
    return candidates[0].expanduser().resolve()


def prepend_ld_preload(env, preload_path):
    existing = env.get("LD_PRELOAD")
    env["LD_PRELOAD"] = f"{preload_path}:{existing}" if existing else str(preload_path)


def effective_preload_path(args, splash_root):
    if args.preload_path:
        return Path(args.preload_path).expanduser().resolve()
    for build in splash_build_candidates(args, splash_root):
        preload = build / "libpgas_preload.so"
        if preload.exists():
            return preload
    return splash_build_candidates(args, splash_root)[0] / "libpgas_preload.so"


def splash_build_candidates(args, splash_root):
    candidates = []
    if args.splash_build:
        candidates.append(Path(args.splash_build))
    if os.environ.get("SPLASH_BUILD"):
        candidates.append(Path(os.environ["SPLASH_BUILD"]))
    candidates.extend([splash_root / "build", ROOT / "build/splash"])
    return [candidate.expanduser().resolve() for candidate in candidates]


def resolve_splash_build(args, splash_root):
    for build in splash_build_candidates(args, splash_root):
        if (build / "libcxl_backend.a").exists() or (build / "cxl_shmem_server").exists():
            return build
    return splash_build_candidates(args, splash_root)[0]


def parse_workloads(text):
    allowed = {"faiss", "vsag", "qdrant"}
    workloads = [item.strip().lower() for item in text.split(",") if item.strip()]
    unknown = sorted(set(workloads) - allowed)
    if unknown:
        raise ValueError(f"unknown workload(s): {', '.join(unknown)}")
    return workloads


def splash_env(args, splash_root, include_preload=True):
    if args.num_nodes < 1 or args.num_nodes > 16:
        raise ValueError("--num-nodes must be between 1 and Splash PGAS_MAX_NODES (16)")

    env = os.environ.copy()
    splash_build = resolve_splash_build(args, splash_root)
    env["SPLASH_ROOT"] = str(splash_root)
    env["SPLASH_BUILD"] = str(splash_build)
    env["PGAS_ENABLED"] = "1"
    env["PGAS_LOCAL_NODE"] = "0"
    env["PGAS_NUM_NODES"] = str(args.num_nodes)
    env["PGAS_CXLMEMSIM_MAX_NODES"] = str(args.num_nodes)
    env["PGAS_BASE"] = args.pgas_base
    env["PGAS_SIZE"] = str(args.pgas_size_mb * 1024 * 1024)
    env["PGAS_MALLOC_REDIRECT"] = "1"
    env["PGAS_MALLOC_THRESHOLD"] = str(args.malloc_threshold)
    env["PGAS_AFFINITY"] = "interleave"
    env["PGAS_CXLMEMSIM_EAGER_CONNECT"] = "0"
    env["ANN_SPLASH_MOUNT_ROOT"] = "1"

    if not args.enable_preload_interceptor:
        env["PGAS_NO_INTERCEPTOR"] = "1"

    for node in range(args.num_nodes):
        env[f"PGAS_CXLMEMSIM_{node}"] = f"{args.server_host}:{args.server_port_base + node}:{node}"

    preload_path = effective_preload_path(args, splash_root)
    if include_preload and args.preload != "off":
        if preload_path.exists():
            prepend_ld_preload(env, preload_path)
            env["ANN_SPLASH_PRELOAD"] = "1"
        elif args.preload == "on":
            raise FileNotFoundError(f"missing PGAS preload library at {preload_path}")

    return env


def run(cmd, cwd, env, dry_run):
    print("+", " ".join(shlex.quote(str(part)) for part in cmd), flush=True)
    if dry_run:
        return
    subprocess.run(cmd, cwd=cwd, env=env, check=True)


def run_faiss(args, splash_root):
    env = splash_env(args, splash_root, include_preload=False)
    cmd = [
        sys.executable,
        str(FAISS_RUNNER),
        "--splash-root",
        str(splash_root),
        "--splash-build",
        str(resolve_splash_build(args, splash_root)),
        "--num-nodes",
        str(args.num_nodes),
        "--pool-provider",
        args.faiss_pool_provider,
        "--nb",
        str(args.faiss_nb),
        "--nq",
        str(args.faiss_nq),
        "--dim",
        str(args.faiss_dim),
        "--k",
        str(args.count),
        "--iters",
        str(args.faiss_iters),
        "--block",
        str(args.faiss_block),
        "--capacity-mb",
        str(args.faiss_capacity_mb),
        "--latency-ns",
        str(args.faiss_latency_ns),
        "--threads",
        str(args.faiss_threads),
        "--pgas-base",
        args.pgas_base,
        "--pgas-size-mb",
        str(args.pgas_size_mb),
        "--malloc-threshold",
        str(args.malloc_threshold),
        "--server-host",
        args.server_host,
        "--server-port-base",
        str(args.server_port_base),
    ]
    preload_path = effective_preload_path(args, splash_root)
    if args.preload != "off" and preload_path.exists():
        cmd.extend(["--pgas-preload", str(preload_path)])
    elif args.preload == "on":
        raise FileNotFoundError(f"missing PGAS preload library at {preload_path}")
    if args.enable_preload_interceptor:
        cmd.append("--enable-preload-interceptor")
    if args.faiss_skip_build:
        cmd.append("--skip-build")
    run(cmd, ROOT, env, args.dry_run)


def run_ann(args, splash_root, algorithm):
    env = splash_env(args, splash_root, include_preload=True)
    cmd = [
        sys.executable,
        "run.py",
        "--dataset",
        args.dataset,
        "--algorithm",
        algorithm,
        "--count",
        str(args.count),
        "--runs",
        str(args.runs),
        "--parallelism",
        "1",
        "--max-n-algorithms",
        str(args.max_n_algorithms),
    ]
    if args.force:
        cmd.append("--force")
    if args.ann_batch:
        cmd.append("--batch")
    if args.ann_local and algorithm != "qdrant":
        cmd.append("--local")
    if algorithm == "qdrant":
        cmd.append("--run-disabled")
    run(cmd, ANN_ROOT, env, args.dry_run)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run FAISS, VSAG, and Qdrant with Splash allocator/PGAS settings."
    )
    parser.add_argument("--workloads", default="faiss,vsag,qdrant")
    parser.add_argument("--splash-root", default=None)
    parser.add_argument("--splash-build", default=None)
    parser.add_argument("--num-nodes", type=int, default=16)
    parser.add_argument("--preload", choices=["auto", "on", "off"], default="auto")
    parser.add_argument("--preload-path", default=None)
    parser.add_argument("--pgas-base", default="0x100000000")
    parser.add_argument("--pgas-size-mb", type=int, default=4096)
    parser.add_argument("--malloc-threshold", type=int, default=65536)
    parser.add_argument("--server-host", default="127.0.0.1")
    parser.add_argument("--server-port-base", type=int, default=9000)
    parser.add_argument("--enable-preload-interceptor", action="store_true")
    parser.add_argument("--dataset", default="random-xs-20-angular")
    parser.add_argument("--count", type=int, default=10)
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--max-n-algorithms", type=int, default=1)
    parser.add_argument("--ann-local", action="store_true", help="Run VSAG locally instead of through Docker.")
    parser.add_argument("--ann-batch", dest="ann_batch", action="store_true", default=True)
    parser.add_argument("--no-ann-batch", dest="ann_batch", action="store_false")
    parser.add_argument("--force", dest="force", action="store_true", default=True)
    parser.add_argument("--no-force", dest="force", action="store_false")
    parser.add_argument("--faiss-pool-provider", choices=["splash", "cxlmemsim"], default="splash")
    parser.add_argument("--faiss-nb", type=int, default=10000)
    parser.add_argument("--faiss-nq", type=int, default=64)
    parser.add_argument("--faiss-dim", type=int, default=64)
    parser.add_argument("--faiss-iters", type=int, default=1)
    parser.add_argument("--faiss-block", type=int, default=2048)
    parser.add_argument("--faiss-capacity-mb", type=int, default=256)
    parser.add_argument("--faiss-latency-ns", type=int, default=100)
    parser.add_argument("--faiss-threads", type=int, default=1)
    parser.add_argument("--faiss-skip-build", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    splash_root = resolve_splash_root(args.splash_root)
    if not splash_root.exists():
        raise FileNotFoundError(f"missing Splash checkout at {splash_root}")
    if not ANN_ROOT.exists():
        raise FileNotFoundError(f"missing ANN-Benchmarks checkout at {ANN_ROOT}")

    workloads = parse_workloads(args.workloads)
    for workload in workloads:
        if workload == "faiss":
            run_faiss(args, splash_root)
        else:
            run_ann(args, splash_root, workload)


if __name__ == "__main__":
    main()
