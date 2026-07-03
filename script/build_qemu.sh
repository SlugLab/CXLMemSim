#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
qemu_root="$repo_root/lib/qemu"
build_dir="${QEMU_BUILD_DIR:-$qemu_root/build}"
meson_wheel="$qemu_root/python/wheels/meson-1.8.1-py3-none-any.whl"
hetgpu="${HETGPU_BUILD:-0}"
wipe="${QEMU_BUILD_WIPE:-0}"
jobs="${JOBS:-$(nproc)}"
configure_args=("--disable-werror")
supports_hetgpu_option=0

if [[ ! -f "$meson_wheel" ]]; then
  echo "missing vendored Meson wheel: $meson_wheel" >&2
  exit 1
fi

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "missing required command: $1" >&2
    return 1
  fi
}

need_cmd python3
need_cmd ninja
need_cmd cc

if [[ "$wipe" == "1" && -d "$build_dir" ]]; then
  rm -rf "$build_dir"
fi

if grep -q "option('hetgpu'" "$qemu_root/meson_options.txt"; then
  supports_hetgpu_option=1
fi

if [[ "$supports_hetgpu_option" == "1" ]]; then
  if [[ "$hetgpu" == "1" ]]; then
    need_cmd cargo
    configure_args+=("-Dhetgpu=enabled")
  else
    configure_args+=("-Dhetgpu=disabled")
  fi
elif [[ "$hetgpu" == "1" ]]; then
  echo "warning: this QEMU tree does not expose a Meson hetgpu option; building with in-tree defaults" >&2
fi

mkdir -p "$build_dir"
cd "$build_dir"

export PYTHONPATH="$meson_wheel${PYTHONPATH:+:$PYTHONPATH}"

../configure "${configure_args[@]}"

ninja -C "$build_dir" -j "$jobs"
