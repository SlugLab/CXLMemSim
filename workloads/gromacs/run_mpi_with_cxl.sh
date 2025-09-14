#!/bin/bash

# Script to run MPI with CXL shim inside QEMU
# This ensures LD_PRELOAD is properly set and propagated

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Path to the CXL shim library
CXL_SHIM_LIB="${SCRIPT_DIR}/libmpi_cxl_shim.so"

# Check if the shim library exists
if [ ! -f "$CXL_SHIM_LIB" ]; then
    echo "Error: CXL shim library not found at $CXL_SHIM_LIB"
    echo "Building the library..."
    cd "$SCRIPT_DIR"
    mpicc -shared -fPIC -o libmpi_cxl_shim.so mpi_cxl_shim.c -ldl -pthread
    if [ $? -ne 0 ]; then
        echo "Failed to build CXL shim library"
        exit 1
    fi
    echo "CXL shim library built successfully"
fi

# Verify the library has correct symbols
echo "Checking library symbols..."
if ! nm -D "$CXL_SHIM_LIB" | grep -q "T MPI_Init"; then
    echo "Warning: MPI_Init symbol not found in shim library"
fi

# Set environment variables for CXL shim
export CXL_SHIM_VERBOSE=1
export CXL_SHIM_ALLOC=1
export CXL_SHIM_WIN=1
export CXL_SHIM_COPY_SEND=1
export CXL_SHIM_COPY_RECV=1

# Use shared memory instead of DAX if not available
if [ ! -e "/dev/dax0.0" ]; then
    echo "DAX device not found, using shared memory fallback"
    export CXL_MEM_SIZE=$((4*1024*1024*1024))  # 4GB
else
    export CXL_DAX_PATH="/dev/dax0.0"
fi

# Debug: Check if we're in QEMU
if [ -f /proc/cpuinfo ] && grep -q "QEMU" /proc/cpuinfo; then
    echo "Running inside QEMU VM"
fi

# Debug: Print environment
echo "Environment setup:"
echo "  LD_PRELOAD=$CXL_SHIM_LIB"
echo "  CXL_SHIM_VERBOSE=$CXL_SHIM_VERBOSE"
echo "  CXL_DAX_PATH=${CXL_DAX_PATH:-not set}"
echo "  CXL_MEM_SIZE=${CXL_MEM_SIZE:-not set}"

# Create a wrapper script for debugging
cat > /tmp/mpi_wrapper.sh << 'WRAPPER_EOF'
#!/bin/bash
echo "[WRAPPER] LD_PRELOAD=$LD_PRELOAD"
echo "[WRAPPER] Running: $@"

# Check if the library is actually loaded
if [ -n "$LD_PRELOAD" ]; then
    echo "[WRAPPER] Checking if library is loadable..."
    ldd "$LD_PRELOAD" > /dev/null 2>&1
    if [ $? -eq 0 ]; then
        echo "[WRAPPER] Library dependencies satisfied"
    else
        echo "[WRAPPER] Warning: Library dependencies might not be satisfied"
        ldd "$LD_PRELOAD" 2>&1
    fi
fi

# Run the actual command
exec "$@"
WRAPPER_EOF
chmod +x /tmp/mpi_wrapper.sh

# Method 1: Direct LD_PRELOAD
echo ""
echo "=== Method 1: Direct LD_PRELOAD ==="
LD_PRELOAD="$CXL_SHIM_LIB" mpirun --allow-run-as-root -np 2 \
    -x LD_PRELOAD="$CXL_SHIM_LIB" \
    -x CXL_SHIM_VERBOSE \
    -x CXL_DAX_PATH \
    -x CXL_MEM_SIZE \
    -x CXL_SHIM_ALLOC \
    -x CXL_SHIM_WIN \
    -hostfile hostfile \
    /tmp/mpi_wrapper.sh ./gmx_mpi mdrun -s benchMEM.tpr -nsteps 10000 -resethway -ntomp 1

# Method 2: Using patchelf to add needed library
if command -v patchelf > /dev/null 2>&1; then
    echo ""
    echo "=== Method 2: Using patchelf ==="

    # Create a copy of gmx_mpi
    cp ./gmx_mpi ./gmx_mpi_patched

    # Add the shim library as a needed library
    patchelf --add-needed "$CXL_SHIM_LIB" ./gmx_mpi_patched

    # Run without LD_PRELOAD
    mpirun --allow-run-as-root -np 2 \
        -x CXL_SHIM_VERBOSE \
        -x CXL_DAX_PATH \
        -x CXL_MEM_SIZE \
        -hostfile hostfile \
        ./gmx_mpi_patched mdrun -s benchMEM.tpr -nsteps 10000 -resethway -ntomp 1
fi

# Method 3: Using a launcher script
echo ""
echo "=== Method 3: Using launcher script ==="
cat > /tmp/launch_gmx.sh << LAUNCHER_EOF
#!/bin/bash
export LD_PRELOAD="$CXL_SHIM_LIB"
echo "[LAUNCHER] Process \$\$: LD_PRELOAD=\$LD_PRELOAD"

# Verify library is loaded
if ldd \$0 2>/dev/null | grep -q "libmpi_cxl_shim"; then
    echo "[LAUNCHER] CXL shim library is loaded"
else
    echo "[LAUNCHER] Warning: CXL shim library might not be loaded"
fi

exec ./gmx_mpi "\$@"
LAUNCHER_EOF
chmod +x /tmp/launch_gmx.sh

mpirun --allow-run-as-root -np 2 \
    -hostfile hostfile \
    /tmp/launch_gmx.sh mdrun -s benchMEM.tpr -nsteps 10000 -resethway -ntomp 1

# Method 4: Using LD_LIBRARY_PATH
echo ""
echo "=== Method 4: Using LD_LIBRARY_PATH ==="
export LD_LIBRARY_PATH="$SCRIPT_DIR:$LD_LIBRARY_PATH"
mpirun --allow-run-as-root -np 2 \
    -x LD_LIBRARY_PATH \
    -x LD_PRELOAD="libmpi_cxl_shim.so" \
    -x CXL_SHIM_VERBOSE \
    -hostfile hostfile \
    ./gmx_mpi mdrun -s benchMEM.tpr -nsteps 10000 -resethway -ntomp 1