#!/usr/bin/env python3
"""Build and run cxltime SlugAllocator workloads with CXLMemSim."""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
IS_DARWIN = platform.system() == "Darwin"
DEFAULT_BUILD_NAME = "build-mac-slug" if IS_DARWIN else "build-slug"


def run_cmd(cmd: list[str], *, env: dict[str, str] | None = None) -> None:
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, env=env)


def brew_prefix(name: str) -> Path | None:
    brew = shutil.which("brew")
    if not brew:
        return None
    try:
        out = subprocess.check_output([brew, "--prefix", name], text=True).strip()
    except (subprocess.CalledProcessError, OSError):
        return None
    return Path(out) if out else None


def resolve_path(path: str | None) -> Path | None:
    if not path:
        return None
    return Path(path).expanduser().resolve()


def default_cxltime_root() -> Path | None:
    env_root = resolve_path(os.environ.get("CXLTIME_ROOT"))
    if env_root:
        return env_root
    sibling = REPO_ROOT.parent / "cxltime"
    if sibling.exists():
        return sibling.resolve()
    return None


def default_build_dir(cxltime_root: Path | None) -> Path | None:
    env_build = resolve_path(os.environ.get("SLUG_BUILD_DIR"))
    if env_build:
        return env_build
    if cxltime_root:
        return (cxltime_root / DEFAULT_BUILD_NAME).resolve()
    return None


def default_llvm_dir() -> Path | None:
    env_llvm = resolve_path(os.environ.get("LLVM_DIR"))
    if env_llvm:
        return env_llvm
    for formula in ("llvm@21", "llvm"):
        prefix = brew_prefix(formula)
        if not prefix:
            continue
        candidate = prefix / "lib" / "cmake" / "llvm"
        if (candidate / "LLVMConfig.cmake").exists():
            return candidate.resolve()
    for candidate in (
        Path("/opt/homebrew/opt/llvm@21/lib/cmake/llvm"),
        Path("/usr/local/opt/llvm@21/lib/cmake/llvm"),
        Path("/opt/homebrew/opt/llvm/lib/cmake/llvm"),
        Path("/usr/local/opt/llvm/lib/cmake/llvm"),
    ):
        if (candidate / "LLVMConfig.cmake").exists():
            return candidate.resolve()
    return None


def default_llvm_prefix(llvm_dir: Path | None) -> Path | None:
    env_root = resolve_path(os.environ.get("LLVM_ROOT"))
    if env_root:
        return env_root
    if llvm_dir and llvm_dir.parts[-3:] == ("lib", "cmake", "llvm"):
        return llvm_dir.parents[2]
    for formula in ("llvm@21", "llvm"):
        prefix = brew_prefix(formula)
        if prefix:
            return prefix.resolve()
    return None


def find_clang(llvm_dir: Path | None, cxx: bool, override: str | None) -> str:
    if override:
        return override
    exe = "clang++" if cxx else "clang"
    llvm_prefix = default_llvm_prefix(llvm_dir)
    if llvm_prefix:
        candidate = llvm_prefix / "bin" / exe
        if candidate.exists():
            return str(candidate)
    found = shutil.which(exe)
    if found:
        return found
    raise SystemExit(f"could not find {exe}; pass --clang or set LLVM_ROOT")


def artifact_candidates(build_dir: Path, name: str, suffixes: tuple[str, ...]) -> list[Path]:
    root = build_dir / "tools" / "slug_allocator"
    return [root / f"{name}.{suffix}" for suffix in suffixes]


def first_existing(candidates: list[Path]) -> Path:
    for candidate in candidates:
        if candidate.exists():
            return candidate.resolve()
    return candidates[0].resolve()


def make_context(args: argparse.Namespace) -> dict[str, Path | None]:
    cxltime_root = resolve_path(getattr(args, "cxltime_root", None)) or default_cxltime_root()
    build_dir = resolve_path(getattr(args, "build_dir", None)) or default_build_dir(cxltime_root)
    llvm_dir = resolve_path(getattr(args, "llvm_dir", None)) or default_llvm_dir()

    requested_pass_path = resolve_path(getattr(args, "pass_path", None))
    requested_runtime_path = resolve_path(getattr(args, "runtime", None))
    pass_path = requested_pass_path if requested_pass_path and requested_pass_path.exists() else None
    runtime_path = (
        requested_runtime_path
        if requested_runtime_path and requested_runtime_path.exists()
        else None
    )
    if build_dir:
        if not pass_path:
            pass_path = first_existing(
                artifact_candidates(build_dir, "SlugAllocatorPass", ("dylib", "so"))
            )
        if not runtime_path:
            runtime_path = first_existing(
                artifact_candidates(
                    build_dir, "libslug_allocator_runtime", ("dylib", "so")
                )
            )

    return {
        "cxltime_root": cxltime_root,
        "build_dir": build_dir,
        "llvm_dir": llvm_dir,
        "pass_path": pass_path,
        "runtime_path": runtime_path,
    }


def ensure_context(ctx: dict[str, Path | None]) -> tuple[Path, Path, Path | None]:
    cxltime_root = ctx["cxltime_root"]
    build_dir = ctx["build_dir"]
    if not cxltime_root:
        raise SystemExit("cxltime checkout not found; pass --cxltime-root")
    if not (cxltime_root / "tools" / "slug_allocator" / "CMakeLists.txt").exists():
        raise SystemExit(f"{cxltime_root} does not contain tools/slug_allocator")
    if not build_dir:
        raise SystemExit("SlugAllocator build directory not found; pass --build-dir")
    return cxltime_root, build_dir, ctx["llvm_dir"]


def build_tools(args: argparse.Namespace, ctx: dict[str, Path | None]) -> None:
    cxltime_root, build_dir, llvm_dir = ensure_context(ctx)
    configure_cmd = [
        "cmake",
        "-S",
        str(cxltime_root),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        "-DBPFTIME_BUILD_WITH_LIBBPF=OFF",
        "-DBPFTIME_BUILD_KERNEL_BPF=OFF",
        "-DBUILD_BPFTIME_DAEMON=OFF",
        "-DBPFTIME_LLVM_JIT=OFF",
        "-DBPFTIME_UBPF_JIT=OFF",
        "-DBPFTIME_BUILD_RUNTIME=OFF",
        "-DBPFTIME_BUILD_ATTACH=OFF",
        "-DBPFTIME_BUILD_BPFTIME_TOOLS=OFF",
        "-DBPFTIME_BUILD_SLUG_ALLOCATOR=ON",
    ]
    if llvm_dir:
        configure_cmd.append(f"-DLLVM_DIR={llvm_dir}")
    run_cmd(configure_cmd)
    jobs = str(getattr(args, "jobs", None) or os.cpu_count() or 2)
    run_cmd(
        [
            "cmake",
            "--build",
            str(build_dir),
            "--target",
            "SlugAllocatorPass",
            "slug_allocator_runtime",
            "-j",
            jobs,
        ]
    )


def require_artifacts(ctx: dict[str, Path | None]) -> tuple[Path, Path]:
    pass_path = ctx["pass_path"]
    runtime_path = ctx["runtime_path"]
    missing = []
    if not pass_path or not pass_path.exists():
        missing.append(f"pass={pass_path}")
    if not runtime_path or not runtime_path.exists():
        missing.append(f"runtime={runtime_path}")
    if missing:
        raise SystemExit(
            "SlugAllocator artifacts missing; run `cxlmemsim_slug.py build-tools` "
            "or pass --build-tools. Missing: " + ", ".join(missing)
        )
    return pass_path, runtime_path


def split_remainder(values: list[str], what: str) -> tuple[list[str], list[str]]:
    if values and values[0] == "--":
        values = values[1:]
    if "--" in values:
        idx = values.index("--")
        left = values[:idx]
        right = values[idx + 1 :]
    else:
        left = values
        right = []
    if not left:
        raise SystemExit(f"missing {what}")
    return left, right


def compile_workload(
    args: argparse.Namespace,
    ctx: dict[str, Path | None],
    sources: list[str],
    clang_args: list[str],
) -> None:
    if getattr(args, "build_tools", False):
        build_tools(args, ctx)
        ctx.update(make_context(args))
    pass_path, runtime_path = require_artifacts(ctx)
    llvm_dir = ctx["llvm_dir"]
    cxx = getattr(args, "cxx", False) or any(
        Path(src).suffix in (".cc", ".cpp", ".cxx", ".C") for src in sources
    )
    compiler = find_clang(llvm_dir, cxx, getattr(args, "clang", None))
    runtime_dir = runtime_path.parent
    output = getattr(args, "output", None)
    if not output:
        raise SystemExit("compile requires -o/--output")
    if not clang_args:
        clang_args = ["-O1", "-g"]
    cmd = [
        compiler,
        f"-fpass-plugin={pass_path}",
        *sources,
        *clang_args,
        f"-L{runtime_dir}",
        "-lslug_allocator_runtime",
        f"-Wl,-rpath,{runtime_dir}",
        "-o",
        output,
    ]
    run_cmd(cmd)


def prepend_env_path(env: dict[str, str], key: str, value: Path) -> None:
    existing = env.get(key)
    env[key] = str(value) if not existing else f"{value}{os.pathsep}{existing}"


def run_workload(args: argparse.Namespace, ctx: dict[str, Path | None]) -> int:
    program, _ = split_remainder(args.program, "program")
    _, runtime_path = require_artifacts(ctx)
    env = os.environ.copy()
    runtime_dir = runtime_path.parent
    prepend_env_path(env, "DYLD_LIBRARY_PATH", runtime_dir)
    prepend_env_path(env, "DYLD_FALLBACK_LIBRARY_PATH", runtime_dir)
    prepend_env_path(env, "LD_LIBRARY_PATH", runtime_dir)

    if args.trace:
        env["SLUG_TRACE"] = str(Path(args.trace).expanduser())
    if args.host:
        env["SLUG_CXL_HOST"] = args.host
    if args.port is not None:
        env["SLUG_CXL_PORT"] = str(args.port)
    if args.region_base is not None:
        env["SLUG_REGION_BASE"] = args.region_base
    if args.region_size is not None:
        env["SLUG_REGION_SIZE"] = args.region_size
    if args.cxl_addr_base is not None:
        env["SLUG_CXL_ADDR_BASE"] = args.cxl_addr_base
    if args.region_only:
        env["SLUG_TRACE_ALL"] = "0"
    if args.verbose:
        env["SLUG_VERBOSE"] = "1"
    if args.no_stats:
        env["SLUG_STATS"] = "0"

    print("+ " + " ".join(program), flush=True)
    return subprocess.run(program, env=env).returncode


def smoke(args: argparse.Namespace, ctx: dict[str, Path | None]) -> None:
    build_tools(args, ctx)
    ctx.update(make_context(args))
    with tempfile.TemporaryDirectory(prefix="cxlmemsim-slug-") as tmp:
        tmpdir = Path(tmp)
        src = tmpdir / "slug_smoke.c"
        exe = tmpdir / "slug_smoke"
        trace = tmpdir / "slug.csv"
        src.write_text(
            """
#include <stdint.h>
#include <stdlib.h>

int main(void) {
    volatile uint64_t *buf = (volatile uint64_t *)calloc(128, sizeof(uint64_t));
    if (!buf) return 2;
    for (int i = 0; i < 128; ++i) buf[i] = (uint64_t)i;
    uint64_t sum = 0;
    for (int i = 0; i < 128; ++i) sum += buf[i];
    free((void *)buf);
    return sum == 8128 ? 0 : 1;
}
""".lstrip()
        )
        compile_args = argparse.Namespace(**vars(args))
        compile_args.output = str(exe)
        compile_args.cxx = False
        compile_args.clang = getattr(args, "clang", None)
        compile_args.build_tools = False
        compile_workload(compile_args, ctx, [str(src)], ["-O1", "-g"])
        run_args = argparse.Namespace(
            program=[str(exe)],
            trace=str(trace),
            host=None,
            port=None,
            region_base=None,
            region_size=None,
            cxl_addr_base=None,
            region_only=False,
            verbose=False,
            no_stats=True,
        )
        rc = run_workload(run_args, ctx)
        if rc != 0:
            raise SystemExit(f"smoke binary exited with {rc}")
        lines = trace.read_text().splitlines() if trace.exists() else []
        if len(lines) <= 1:
            raise SystemExit(f"smoke trace is empty: {trace}")
        print(f"smoke ok: {len(lines) - 1} memory events in {trace}")


def print_paths(ctx: dict[str, Path | None]) -> None:
    payload = {
        key: str(value) if value else None
        for key, value in ctx.items()
    }
    payload["pass_exists"] = bool(ctx["pass_path"] and ctx["pass_path"].exists())
    payload["runtime_exists"] = bool(
        ctx["runtime_path"] and ctx["runtime_path"].exists()
    )
    print(json.dumps(payload, indent=2, sort_keys=True))


def add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--cxltime-root", help="Path to the cxltime checkout")
    parser.add_argument("--build-dir", help="cxltime SlugAllocator build directory")
    parser.add_argument("--llvm-dir", help="LLVMConfig.cmake directory")
    parser.add_argument("--pass", dest="pass_path", help="SlugAllocatorPass plugin")
    parser.add_argument("--runtime", help="SlugAllocator runtime library")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compile and run workloads with cxltime SlugAllocator and CXLMemSim"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_paths = sub.add_parser("paths", help="Print discovered paths")
    add_common(p_paths)

    p_build = sub.add_parser("build-tools", help="Build the cxltime SlugAllocator")
    add_common(p_build)
    p_build.add_argument("-j", "--jobs", type=int, help="Parallel build jobs")

    p_compile = sub.add_parser("compile", help="Compile a Slug-instrumented binary")
    add_common(p_compile)
    p_compile.add_argument("-o", "--output", required=True, help="Output binary")
    p_compile.add_argument("--clang", help="clang or clang++ executable")
    p_compile.add_argument("--cxx", action="store_true", help="Use clang++")
    p_compile.add_argument(
        "--build-tools", action="store_true", help="Build SlugAllocator first"
    )
    p_compile.add_argument("-j", "--jobs", type=int, help="Parallel build jobs")
    p_compile.add_argument(
        "compile_args",
        nargs=argparse.REMAINDER,
        help="sources, then optional `--` followed by extra clang flags",
    )

    p_run = sub.add_parser("run", help="Run an instrumented binary")
    add_common(p_run)
    p_run.add_argument("--trace", help="Write Slug CSV trace to this path")
    p_run.add_argument("--host", default="127.0.0.1", help="CXLMemSim TCP host")
    p_run.add_argument("--port", type=int, help="CXLMemSim TCP port")
    p_run.add_argument("--region-base", help="Instrumented CXL region base")
    p_run.add_argument("--region-size", help="Instrumented CXL region size")
    p_run.add_argument("--cxl-addr-base", help="CXLMemSim address-base translation")
    p_run.add_argument("--region-only", action="store_true", help="Trace region only")
    p_run.add_argument("--verbose", action="store_true", help="Enable Slug logs")
    p_run.add_argument("--no-stats", action="store_true", help="Disable exit stats")
    p_run.add_argument("program", nargs=argparse.REMAINDER)

    p_smoke = sub.add_parser("smoke", help="Build and run a local smoke test")
    add_common(p_smoke)
    p_smoke.add_argument("--clang", help="clang executable")
    p_smoke.add_argument("-j", "--jobs", type=int, help="Parallel build jobs")

    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    ctx = make_context(args)
    if args.command == "paths":
        print_paths(ctx)
        return 0
    if args.command == "build-tools":
        build_tools(args, ctx)
        return 0
    if args.command == "compile":
        sources, clang_args = split_remainder(args.compile_args, "source files")
        compile_workload(args, ctx, sources, clang_args)
        return 0
    if args.command == "run":
        return run_workload(args, ctx)
    if args.command == "smoke":
        smoke(args, ctx)
        return 0
    raise SystemExit(f"unknown command: {args.command}")


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
