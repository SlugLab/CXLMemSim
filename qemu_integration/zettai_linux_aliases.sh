#!/usr/bin/env bash
set -euo pipefail

ALIAS_ROOT=${ZETTAI_ALIAS_ROOT:-/run/zettai/cxl}
SERVICE_PATH=/etc/systemd/system/zettai-cxl-aliases.service
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

usage() {
    cat <<'EOF'
Usage: zettai_linux_aliases.sh [--install-systemd]

Create guest-side Zettai aliases for standard Linux CXL sysfs names.
Linux still names devices mem0, port0, region0, and decoderX.Y; this script
creates stable symlinks under /run/zettai/cxl and writes a topology summary.
EOF
}

install_systemd() {
    if [[ $EUID -ne 0 ]]; then
        echo "--install-systemd requires root" >&2
        exit 1
    fi

    cat > "$SERVICE_PATH" <<EOF
[Unit]
Description=Create Zettai CXL aliases
After=systemd-udev-settle.service

[Service]
Type=oneshot
ExecStart=$SCRIPT_DIR/$(basename "$0")

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable zettai-cxl-aliases.service
    echo "Installed $SERVICE_PATH"
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi
if [[ "${1:-}" == "--install-systemd" ]]; then
    install_systemd
    exit 0
fi

mkdir -p "$ALIAS_ROOT"
rm -f "$ALIAS_ROOT"/zettai-* "$ALIAS_ROOT"/topology.txt

alias_class() {
    local pattern=$1
    local prefix=$2
    local path
    local name

    for path in /sys/bus/cxl/devices/$pattern; do
        [[ -e "$path" ]] || continue
        name=$(basename "$path")
        ln -sfn "$path" "$ALIAS_ROOT/$prefix-$name"
    done
}

alias_class 'mem*' zettai
alias_class 'region*' zettai
alias_class 'port*' zettai
alias_class 'endpoint*' zettai
alias_class 'decoder*' zettai

{
    echo "Zettai Linux CXL alias snapshot"
    date
    echo
    echo "[PCI 7a74:a123]"
    if command -v lspci >/dev/null 2>&1; then
        lspci -nn | grep -i '7a74:a123' || true
    else
        echo "lspci not installed"
    fi
    echo
    echo "[CXL]"
    if command -v cxl >/dev/null 2>&1; then
        cxl list -B -D -P -M -R -u || true
    else
        echo "cxl not installed"
    fi
    echo
    echo "[DAX]"
    if command -v daxctl >/dev/null 2>&1; then
        daxctl list || true
    else
        echo "daxctl not installed"
    fi
    echo
    echo "[PCI tree]"
    if command -v lspci >/dev/null 2>&1; then
        lspci -tvnn || true
    fi
} > "$ALIAS_ROOT/topology.txt"

echo "Created Zettai aliases in $ALIAS_ROOT"
ls -l "$ALIAS_ROOT"
