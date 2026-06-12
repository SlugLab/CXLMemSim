#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SCRIPT="$SCRIPT_DIR/setup_cxl_numa.sh"
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

STUB_DIR="$TMP_DIR/bin"
STATE_DIR="$TMP_DIR/state"
mkdir -p "$STUB_DIR" "$STATE_DIR"

cat >"$STUB_DIR/cxl" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail

case "$*" in
    "list -M"*)
        echo '[{"memdev":"mem0"}]'
        ;;
    "list -B -D"*)
        echo '[{"decoder":"decoder0.0","volatile_capable":true,"max_available_extent":268435456}]'
        ;;
    "list -R"*)
        if [[ -f "$CXL_TEST_STATE/region_created" ]]; then
            echo '[{"region":"region0"}]'
        else
            echo '[]'
        fi
        ;;
    create-region*)
        touch "$CXL_TEST_STATE/region_created"
        echo '{"region":"region0"}'
        ;;
    list*)
        echo '[]'
        ;;
    *)
        echo "unexpected cxl invocation: $*" >&2
        exit 2
        ;;
esac
STUB

cat >"$STUB_DIR/daxctl" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail

case "${1:-}" in
    list)
        echo '[{"chardev":"dax0.0"}]'
        ;;
    create-device)
        echo '{"chardev":"dax0.0"}'
        ;;
    reconfigure-device)
        exit 0
        ;;
    *)
        echo "unexpected daxctl invocation: $*" >&2
        exit 2
        ;;
esac
STUB

cat >"$STUB_DIR/ip" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail

printf '%s\n' "$*" >>"$CXL_TEST_STATE/ip.calls"

case "$*" in
    "-o link show up"|"-o link show")
        echo '2: enp0s2: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc fq_codel state UP mode DEFAULT group default qlen 1000'
        ;;
    link\ set\ *|addr\ add\ *|route\ add\ *)
        exit 0
        ;;
    *)
        echo "unexpected ip invocation: $*" >&2
        exit 2
        ;;
esac
STUB

cat >"$STUB_DIR/modprobe" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB

cat >"$STUB_DIR/udevadm" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB

cat >"$STUB_DIR/numactl" <<'STUB'
#!/usr/bin/env bash
echo 'available: 1 nodes (0)'
STUB

chmod +x "$STUB_DIR"/*

export PATH="$STUB_DIR:$PATH"
export CXL_TEST_STATE="$STATE_DIR"

LOG_FILE="$TMP_DIR/cxl_numa_setup.log"

LOG_FILE="$LOG_FILE" \
MAX_RETRIES=1 \
RETRY_DELAY=0 \
CXL_CONFIGURE_NET=1 \
CXL_HOST_ID=1 \
bash "$SCRIPT" >"$TMP_DIR/script.stdout" 2>"$TMP_DIR/script.stderr"

if ! grep -q 'addr add 192.168.100.11/24 dev enp0s2' "$STATE_DIR/ip.calls"; then
    echo "FAIL: secondary host id should default to 192.168.100.11/24" >&2
    echo "ip calls:" >&2
    cat "$STATE_DIR/ip.calls" >&2
    echo "setup log:" >&2
    cat "$LOG_FILE" >&2
    exit 1
fi

echo "OK: setup_cxl_numa secondary network defaults"
