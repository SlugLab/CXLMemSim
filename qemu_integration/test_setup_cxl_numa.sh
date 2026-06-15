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
        if [[ "${CXL_TEST_MEM_KIND:-ram}" == "pmem" ]]; then
            echo '[{"memdev":"mem0","pmem_size":1073741824}]'
        else
            echo '[{"memdev":"mem0","ram_size":1073741824}]'
        fi
        ;;
    "list -B -D"*)
        echo '[{"decoder":"decoder0.0","volatile_capable":true,"pmem_capable":true,"max_available_extent":268435456}]'
        ;;
    "list -R"*)
        if [[ -f "$CXL_TEST_STATE/region_created" ]]; then
            echo '[{"region":"region0"}]'
        else
            echo '[]'
        fi
        ;;
    create-region*)
        printf '%s\n' "$*" >>"$CXL_TEST_STATE/cxl.calls"
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
        if [[ "${CXL_TEST_MEM_KIND:-ram}" == "pmem" && ! -f "$CXL_TEST_STATE/namespace_created" ]]; then
            echo '[]'
        else
            echo '[{"chardev":"dax0.0"}]'
        fi
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

cat >"$STUB_DIR/ndctl" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail

case "$*" in
    "list -N -r region0"*)
        if [[ -f "$CXL_TEST_STATE/namespace_created" ]]; then
            echo '[{"dev":"namespace0.0","mode":"devdax"}]'
        else
            echo '[]'
        fi
        ;;
    create-namespace*)
        printf '%s\n' "$*" >>"$CXL_TEST_STATE/ndctl.calls"
        touch "$CXL_TEST_STATE/namespace_created"
        echo '{"dev":"namespace0.0","mode":"devdax"}'
        ;;
    *)
        echo "unexpected ndctl invocation: $*" >&2
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

if ! grep -q 'create-region -m -t ram ' "$STATE_DIR/cxl.calls"; then
    echo "FAIL: ram memdev should create a ram region" >&2
    cat "$STATE_DIR/cxl.calls" >&2
    exit 1
fi

rm -f "$STATE_DIR/region_created" "$STATE_DIR/namespace_created" "$STATE_DIR/cxl.calls" "$STATE_DIR/ndctl.calls"

LOG_FILE="$LOG_FILE" \
MAX_RETRIES=1 \
RETRY_DELAY=0 \
CXL_TEST_MEM_KIND=pmem \
CXL_CONFIGURE_NET=0 \
bash "$SCRIPT" >"$TMP_DIR/script-pmem.stdout" 2>"$TMP_DIR/script-pmem.stderr"

if ! grep -q 'create-region -m -t pmem ' "$STATE_DIR/cxl.calls"; then
    echo "FAIL: pmem-only memdev should create a pmem region" >&2
    cat "$STATE_DIR/cxl.calls" >&2
    exit 1
fi

if ! grep -q 'create-namespace -m devdax -r region0' "$STATE_DIR/ndctl.calls"; then
    echo "FAIL: pmem region should create an ndctl devdax namespace" >&2
    cat "$STATE_DIR/ndctl.calls" >&2
    exit 1
fi

echo "OK: setup_cxl_numa auto region type and network defaults"
