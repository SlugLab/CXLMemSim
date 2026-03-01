#!/bin/bash
# compile_shim.sh - Build MPI CXL Shim with cache coherence and flush/invalidate options
#
# Usage:
#   ./build.sh                    # Default: build all 7 variants (1 nocc + 6 cc)
#   ./build.sh nocc               # Only nocc (no cache coherence)
#   ./build.sh cc clwb clflush    # CC on with explicit flush and invalidate
#   ./build.sh all                # Build all 7 variants (1 nocc + 6 cc)
#
# Cache coherence: nocc | cc
# Flush (when cc): clwb | clflush | clflushopt
# Invalidate (when cc): clflush | clflushopt

set -e

ARG1="${1:-all}"
ARG2="${2:-clwb}"
ARG3="${3:-clflush}"

if [[ "$ARG1" == "nocc" ]]; then
    CXL_CC=0
    FLUSH=""
    INV=""
elif [[ "$ARG1" == "cc" ]]; then
    CXL_CC=1
    FLUSH="$ARG2"
    INV="$ARG3"
elif [[ "$ARG1" == "all" ]]; then
    CXL_CC="all"
    FLUSH=""
    INV=""
else
    # Legacy: first two args are flush and inv
    CXL_CC=1
    FLUSH="$ARG1"
    INV="$ARG2"
fi

# Find MPI includes
MPI_INCLUDE=$(mpicc -showme:compile 2>/dev/null || echo "-I/usr/include/mpi")
MPI_LIBS=$(mpicc -showme:link 2>/dev/null || echo "-lmpi")

CFLAGS="-shared -fPIC -g -O2 -D_GNU_SOURCE -Wall -Wextra"
LDFLAGS="-ldl -lpthread -lrt $MPI_LIBS"

build_nocc() {
    local lib="libmpi_cxl_shim_nocc.so"
    echo "Building $lib (no cache coherence)..."
    gcc $CFLAGS $MPI_INCLUDE -o "$lib" mpi_cxl_shim.c $LDFLAGS
    echo "  -> $lib"
}

build_cc() {
    local flush="$1"
    local inv="$2"
    local lib="libmpi_cxl_shim_cc_${flush}_${inv}.so"
    local def_cc="CXL_CACHE_COHERENCE"
    local def_flush="CXL_FLUSH_$(echo $flush | tr a-z A-Z)"
    local def_inv="CXL_INV_$(echo $inv | tr a-z A-Z)"

    echo "Building $lib (cache_coherence=on, flush=$flush, invalidate=$inv)..."
    gcc $CFLAGS $MPI_INCLUDE \
        -D$def_cc -D$def_flush -D$def_inv \
        -o "$lib" \
        mpi_cxl_shim.c \
        $LDFLAGS
    echo "  -> $lib"
}

if [[ "$CXL_CC" == "all" ]]; then
    echo "Building all 7 shim variants..."
    build_nocc
    build_cc clwb clflush
    build_cc clwb clflushopt
    build_cc clflush clflush
    build_cc clflush clflushopt
    build_cc clflushopt clflush
    build_cc clflushopt clflushopt
    ln -sf libmpi_cxl_shim_nocc.so libmpi_cxl_shim.so 2>/dev/null || true
    echo "Done. Built: libmpi_cxl_shim_nocc.so + 6 cc variants"
elif [[ "$CXL_CC" == "0" ]]; then
    build_nocc
    ln -sf libmpi_cxl_shim_nocc.so libmpi_cxl_shim.so 2>/dev/null || cp libmpi_cxl_shim_nocc.so libmpi_cxl_shim.so
    echo "Compilation successful! Library: libmpi_cxl_shim_nocc.so"
    echo -e "\nTesting library load..."
    LD_PRELOAD=./libmpi_cxl_shim_nocc.so ls
else
    # Validate when CXL_CC=1
    case "$FLUSH" in clwb|clflush|clflushopt) ;; *)
        echo "Error: FLUSH must be clwb, clflush, or clflushopt" >&2; exit 1;;
    esac
    case "$INV" in clflush|clflushopt) ;; *)
        echo "Error: INV must be clflush or clflushopt" >&2; exit 1;;
    esac

    build_cc "$FLUSH" "$INV"
    LIB="libmpi_cxl_shim_cc_${FLUSH}_${INV}.so"
    echo "Compilation successful!"
    echo "Library: $LIB"
    echo -e "\nExported symbols:"
    nm -D "$LIB" | grep " T " | grep MPI_ | head -10
    echo -e "\nTesting library load..."
    LD_PRELOAD=./"$LIB" ls
fi

