#!/bin/bash

# Script to launch QEMU with keyboard hook enabled

# Check if the library is built
if [ ! -f "build/libCXLMemSim.so" ]; then
    echo "Error: libCXLMemSim.so not found. Please build the project first."
    echo "Run: cd build && cmake .. && make"
    exit 1
fi

# Set environment variables
export CXL_MEMSIM_HOST=${CXL_MEMSIM_HOST:-"127.0.0.1"}
export CXL_MEMSIM_PORT=${CXL_MEMSIM_PORT:-"9999"}
export KBD_HOOK_DEBUG=1

# Add CXLMemSim library to LD_PRELOAD (now includes kbd_hook functionality)
export LD_PRELOAD="$(pwd)/build/libCXLMemSim.so:$LD_PRELOAD"

echo "Starting QEMU with keyboard hook enabled..."
echo "CXL Memory Simulator: $CXL_MEMSIM_HOST:$CXL_MEMSIM_PORT"
echo "LD_PRELOAD: $LD_PRELOAD"
echo ""
echo "The hook will intercept kbd_read_data() calls and check for back invalidations"
echo "Debug output will show all keyboard read operations"
echo ""

# Launch QEMU with your desired configuration
# Example for x86_64 with CXL device:
exec qemu-system-x86_64 \
    -machine q35,cxl=on \
    -m 4G \
    -enable-kvm \
    -cpu host \
    -smp 4 \
    -drive file=disk.img,format=raw,if=virtio \
    "$@"