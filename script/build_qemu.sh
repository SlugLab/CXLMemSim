#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
qemu_root="$repo_root/lib/qemu"
build_dir="${QEMU_BUILD_DIR:-$qemu_root/build}"
meson_wheel="$qemu_root/python/wheels/meson-1.8.1-py3-none-any.whl"
hetgpu="${HETGPU_BUILD:-0}"
wipe="${QEMU_BUILD_WIPE:-0}"
jobs="${JOBS:-$(nproc)}"
target_list="${QEMU_TARGET_LIST:-x86_64-softmmu}"
ninja_targets="${QEMU_NINJA_TARGETS:-qemu-system-x86_64}"
configure_args=()
ninja_target_args=()

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

if grep -q "^option('hetgpu'" "$qemu_root/meson_options.txt"; then
  if [[ "$hetgpu" == "1" ]]; then
    need_cmd cargo
    configure_args+=("-Dhetgpu=enabled")
  else
    configure_args+=("-Dhetgpu=disabled")
  fi
elif [[ "$hetgpu" == "1" ]]; then
  echo "QEMU checkout does not define Meson option 'hetgpu'; building with its default CXL sources" >&2
fi

if [[ -n "$target_list" && "$target_list" != "all" ]]; then
  configure_args+=("--target-list=$target_list")
fi

if [[ -n "$ninja_targets" && "$ninja_targets" != "all" ]]; then
  read -r -a ninja_target_args <<< "$ninja_targets"
fi

mkdir -p "$build_dir"
cd "$build_dir"

PYTHONPATH="$meson_wheel" \
../configure "${configure_args[@]}"

ninja -C "$build_dir" -j "$jobs" "${ninja_target_args[@]}"
