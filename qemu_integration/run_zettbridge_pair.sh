#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
driver_dir=${ZETTBRIDGE_DIR:-"$HOME/zettbridge"}
extra_kernel_args=()
if [[ -n ${PROVIDER_KDIR:-} ]]; then
    extra_kernel_args+=(--provider-kernel "$PROVIDER_KDIR")
fi
if [[ -n ${CONSUMER_KDIR:-} ]]; then
    extra_kernel_args+=(--consumer-kernel "$CONSUMER_KDIR")
fi
if [[ -n ${PROVIDER_ZETTBRIDGE_KO:-} ]]; then
    extra_kernel_args+=(--provider-module "$PROVIDER_ZETTBRIDGE_KO")
fi
if [[ -n ${CONSUMER_ZETTBRIDGE_KO:-} ]]; then
    extra_kernel_args+=(--consumer-module "$CONSUMER_ZETTBRIDGE_KO")
fi
exec python3 "$driver_dir/tools/run_pair.py" \
    --kernel "${KDIR:-$HOME/linux}" \
    --qemu "${QEMU_BINARY:-$repo_dir/lib/qemu/build/qemu-system-x86_64}" \
    "${extra_kernel_args[@]}" "$@"
