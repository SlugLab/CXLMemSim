#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    cat <<'EOF'
Usage: zettai_guest_dcd_gfam_test.sh

Run inside the Linux guest after the host binds a Zettai endpoint and adds DCD
capacity. This wrapper reuses setup_cxl_numa.sh with DCD/GFAM smoke-test
defaults: CXL RAM region, device-dax creation, and /dev/daxX.Y page touches.

Environment:
  REGION_SIZE                 Region size, default: 256M
  INTERLEAVE_GRANULARITY      cxl create-region granularity, default: 1024
  CXL_REGION_TYPE             Region type, default: ram
  ZETTAI_MEMDEV               Override detected memdev, e.g. mem0
  ZETTAI_DECODER              Override detected root decoder, e.g. decoder0.0
  ZETTAI_CREATE_DAX           Create/use device-dax, default: 1
  ZETTAI_TOUCH_DAX            mmap and touch /dev/daxX.Y, default: 1

Host-side preparation example:
  python3 qemu_integration/zettai_host_dcd_gfam_test.py --bind --add --query
EOF
    exit 0
fi

export CXL_REGION_TYPE=${CXL_REGION_TYPE:-ram}
export CXL_CREATE_DAX=${CXL_CREATE_DAX:-${ZETTAI_CREATE_DAX:-1}}
export CXL_DAX_MODE=${CXL_DAX_MODE:-devdax}
export CXL_TOUCH_DAX=${CXL_TOUCH_DAX:-${ZETTAI_TOUCH_DAX:-1}}

exec "$SCRIPT_DIR/setup_cxl_numa.sh" "$@"
