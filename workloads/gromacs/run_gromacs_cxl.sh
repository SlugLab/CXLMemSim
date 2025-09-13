#!/bin/bash

# GROMACS CXL Memory Shim Runner Script
# Usage: ./run_gromacs_cxl.sh [options] <gromacs_args>

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHIM_LIB="${SCRIPT_DIR}/libmpi_cxl_shim.so"

# Default values
USE_DAX=0
DAX_PATH="/dev/dax0.0"
MEM_SIZE=$((4*1024*1024*1024))  # 4GB default
VERBOSE=0
ENABLE_ALLOC=1
ENABLE_WIN=1
ENABLE_COPY=0
NUM_PROCS=4

# Parse command line options
while [[ $# -gt 0 ]]; do
    case $1 in
        --dax)
            USE_DAX=1
            shift
            if [[ $# -gt 0 && ! "$1" =~ ^-- ]]; then
                DAX_PATH="$1"
                shift
            fi
            ;;
        --mem-size)
            MEM_SIZE="$2"
            shift 2
            ;;
        --verbose)
            VERBOSE=1
            shift
            ;;
        --no-alloc)
            ENABLE_ALLOC=0
            shift
            ;;
        --no-win)
            ENABLE_WIN=0
            shift
            ;;
        --copy)
            ENABLE_COPY=1
            shift
            ;;
        --np)
            NUM_PROCS="$2"
            shift 2
            ;;
        --help)
            cat << EOF
Usage: $0 [options] -- <gromacs_args>

Options:
    --dax [path]     Use DAX device (default: /dev/dax0.0)
    --mem-size SIZE  Set CXL memory size in bytes (default: 4GB)
    --verbose        Enable verbose output
    --no-alloc       Disable MPI_Alloc_mem redirection
    --no-win         Disable MPI window redirection
    --copy           Enable buffer copying for send/recv
    --np N           Number of MPI processes (default: 4)
    --help           Show this help message

Examples:
    # Run with DAX device
    $0 --dax /dev/dax0.0 --verbose -- mdrun -s input.tpr

    # Run with shared memory, 8GB size
    $0 --mem-size \$((8*1024*1024*1024)) -- mdrun -s input.tpr

    # Run with all features enabled
    $0 --dax --copy --verbose -- mdrun -s input.tpr -nsteps 10000
EOF
            exit 0
            ;;
        --)
            shift
            break
            ;;
        *)
            break
            ;;
    esac
done

# Build shim library if not exists
if [ ! -f "$SHIM_LIB" ]; then
    echo "Building MPI CXL shim library..."
    make -C "$SCRIPT_DIR" libmpi_cxl_shim.so || {
        echo "Failed to build shim library"
        exit 1
    }
fi

# Set up environment
export LD_PRELOAD="$SHIM_LIB"

if [ "$USE_DAX" -eq 1 ]; then
    if [ ! -c "$DAX_PATH" ]; then
        echo "Warning: DAX device $DAX_PATH not found, falling back to shared memory"
    else
        export CXL_DAX_PATH="$DAX_PATH"
        echo "Using DAX device: $DAX_PATH"
    fi
else
    echo "Using shared memory mode (size: $((MEM_SIZE / 1024 / 1024)) MB)"
fi

export CXL_MEM_SIZE="$MEM_SIZE"

[ "$VERBOSE" -eq 1 ] && export CXL_SHIM_VERBOSE=1
[ "$ENABLE_ALLOC" -eq 1 ] && export CXL_SHIM_ALLOC=1
[ "$ENABLE_WIN" -eq 1 ] && export CXL_SHIM_WIN=1
[ "$ENABLE_COPY" -eq 1 ] && {
    export CXL_SHIM_COPY_SEND=1
    export CXL_SHIM_COPY_RECV=1
}

# Print configuration
echo "=== CXL Memory Shim Configuration ==="
echo "Shim library: $SHIM_LIB"
echo "Memory size: $((MEM_SIZE / 1024 / 1024)) MB"
echo "MPI processes: $NUM_PROCS"
echo "Features enabled:"
[ "$ENABLE_ALLOC" -eq 1 ] && echo "  - MPI_Alloc_mem redirection"
[ "$ENABLE_WIN" -eq 1 ] && echo "  - MPI window redirection"
[ "$ENABLE_COPY" -eq 1 ] && echo "  - Buffer copying"
[ "$VERBOSE" -eq 1 ] && echo "  - Verbose logging"
echo "====================================="
echo

# Find GROMACS executable
GMX_MPI=""
for candidate in gmx_mpi mdrun_mpi gmx; do
    if command -v $candidate &> /dev/null; then
        GMX_MPI=$candidate
        break
    fi
done

if [ -z "$GMX_MPI" ]; then
    echo "Error: GROMACS MPI executable not found"
    echo "Please ensure GROMACS is installed with MPI support"
    exit 1
fi

# Run GROMACS with MPI
echo "Running: mpirun -np $NUM_PROCS $GMX_MPI $@"
mpirun --allow-run-as-root -np "$NUM_PROCS" "$GMX_MPI" "$@"

exit_code=$?
echo
echo "GROMACS finished with exit code: $exit_code"
exit $exit_code