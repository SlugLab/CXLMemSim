#!/bin/bash
# Clean CXL DAX device and POSIX shared memory left by mpi_cxl_shim.
# Usage: ./cleanup_cxl_dax.sh [--status] [/dev/daxX.Y]
# Env:   CXL_DAX_PATH (default: /dev/dax0.0)
#
# NOTE: Do NOT source this script. Run it as ./cleanup_cxl_dax.sh

(set -euo pipefail  # run everything in a subshell to be safe

STATUS=0
if [[ "${1:-}" == "--status" ]]; then
    STATUS=1; shift
fi

DAX_PATH="${1:-${CXL_DAX_PATH:-/dev/dax0.0}}"
SHM_PATH="/dev/shm/cxlmemsim_mpi_shared"
CXL_MAGIC="004d454d534c5843"  # "CXLSMEM\0" little-endian

dax_read_magic() {
    python3 -c "
import mmap, os
fd = os.open('$1', os.O_RDONLY)
m = mmap.mmap(fd, 2*1024*1024, mmap.MAP_SHARED, mmap.PROT_READ)
print(m[:8].hex())
os.close(fd)
" 2>/dev/null || echo "unreadable"
}

dax_zero_region() {
    python3 -c "
import mmap, os, sys
CXL_SIZE = 4 * 1024 * 1024 * 1024  # DEFAULT_CXL_SIZE from mpi_cxl_shim.c
CHUNK = 2 * 1024 * 1024            # 2MB aligned to DAX page size
fd = os.open('$1', os.O_RDWR)
m = mmap.mmap(fd, CXL_SIZE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
zero = b'\x00' * CHUNK
for off in range(0, CXL_SIZE, CHUNK):
    m[off:off+CHUNK] = zero
m.close()
os.close(fd)
" 2>/dev/null
}

shm_read_magic() {
    dd if="$1" bs=8 count=1 2>/dev/null | xxd -p 2>/dev/null || echo "unreadable"
}

check_dax() {
    if [ ! -e "$DAX_PATH" ]; then
        echo "[STATUS] DAX not found: $DAX_PATH"; return
    fi
    local raw
    raw=$(dax_read_magic "$DAX_PATH")
    if [ "$raw" = "0000000000000000" ]; then
        echo "[STATUS] DAX $DAX_PATH: CLEAN (header zeroed)"
    elif [ "$raw" = "$CXL_MAGIC" ]; then
        echo "[STATUS] DAX $DAX_PATH: IN USE (magic=CXLSMEM)"
    else
        echo "[STATUS] DAX $DAX_PATH: UNKNOWN (first 8B=$raw)"
    fi
}

check_shm() {
    if [ ! -e "$SHM_PATH" ]; then
        echo "[STATUS] SHM not found: $SHM_PATH (clean)"; return
    fi
    local sz raw
    sz=$(stat -c '%s' "$SHM_PATH" 2>/dev/null || echo 0)
    raw=$(shm_read_magic "$SHM_PATH")
    if [ "$raw" = "$CXL_MAGIC" ]; then
        echo "[STATUS] SHM $SHM_PATH: IN USE (magic=CXLSMEM, size=$((sz/1024/1024))MB)"
    else
        echo "[STATUS] SHM $SHM_PATH: EXISTS (size=$((sz/1024/1024))MB, first 8B=$raw)"
    fi
}

if [ $STATUS -eq 1 ]; then
    check_dax
    check_shm
    exit 0
fi

# Zero the full 4GB CXL region (DEFAULT_CXL_SIZE from mpi_cxl_shim.c)
if [ -e "$DAX_PATH" ]; then
    if [ -w "$DAX_PATH" ]; then
        echo "[....] Zeroing 4GB on $DAX_PATH ..."
        dax_zero_region "$DAX_PATH" && \
            echo "[OK] Reset DAX: $DAX_PATH (4GB zeroed)" || \
            echo "[FAIL] Reset DAX: $DAX_PATH"
    else
        echo "[WARN] No write permission: $DAX_PATH"
    fi
else
    echo "[SKIP] DAX not found: $DAX_PATH"
fi

# Remove shared memory segment
if [ -e "$SHM_PATH" ]; then
    rm -f "$SHM_PATH"
    echo "[OK] Removed SHM: $SHM_PATH"
else
    echo "[SKIP] SHM not found: $SHM_PATH"
fi
)
