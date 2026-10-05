#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
driver_dir=${ZETTBRIDGE_DIR:-"$HOME/zettbridge"}
exec python3 "$driver_dir/tools/run_pair.py" \
    --kernel "${KDIR:-$HOME/linux}" \
    --qemu "${QEMU_BINARY:-$repo_dir/lib/qemu/build/qemu-system-x86_64}" "$@"
