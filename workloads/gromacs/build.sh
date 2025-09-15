#!/bin/bash
# compile_shim.sh

set -e

echo "Compiling CXL MPI Shim library..."

# Find MPI includes
MPI_INCLUDE=$(mpicc -showme:compile 2>/dev/null || echo "-I/usr/include/mpi")
MPI_LIBS=$(mpicc -showme:link 2>/dev/null || echo "-lmpi")

# Compile with debug symbols and optimizations
gcc -shared -fPIC -g -O2 \
    $MPI_INCLUDE \
    -D_GNU_SOURCE \
    -Wall -Wextra \
    -o libmpi_cxl_shim.so \
    mpi_cxl_shim.c \
    -ldl -lpthread -lrt \
    $MPI_LIBS

echo "Compilation successful!"
echo "Library created: libmpi_cxl_shim.so"

# Show symbols
echo -e "\nExported symbols:"
nm -D libmpi_cxl_shim.so | grep " T " | grep MPI_ | head -10
# Test that it loads
echo -e "\nTesting library load..."
LD_PRELOAD=./libmpi_cxl_shim.so ls

