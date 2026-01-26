#!/bin/bash
# Test two distributed CXL memory servers on one host using SHM communication
# Usage: ./run_distributed_shm_test.sh [build_dir]

BUILD_DIR="${1:-$(dirname "$0")/../build}"
SERVER="$BUILD_DIR/cxlmemsim_server"

if [ ! -x "$SERVER" ]; then
    echo "ERROR: Server binary not found at $SERVER"
    echo "Build it first: cd build && cmake .. && make cxlmemsim_server"
    exit 1
fi

# Cleanup from previous runs
cleanup() {
    echo "Cleaning up..."
    kill $PID0 $PID1 2>/dev/null
    wait $PID0 $PID1 2>/dev/null
    rm -f /dev/shm/cxlmemsim_dist2node*
    rm -f /dev/shm/cxlmemsim_dist2node_node*
}
trap cleanup EXIT

DIST_SHM="/cxlmemsim_dist2node"
CAPACITY=64

echo "=== Two-Process Distributed CXL SHM Test ==="
echo "Build dir: $BUILD_DIR"
echo "SHM name: $DIST_SHM"
echo ""

# Start Node 0 (coordinator)
echo "--- Starting Node 0 (coordinator) ---"
CXL_BASE_ADDR=0x100000000 $SERVER \
    --comm-mode distributed \
    --node-id 0 \
    --dist-shm-name "$DIST_SHM" \
    --transport-mode shm \
    --capacity $CAPACITY \
    --port 9990 \
    2>&1 | sed 's/^/[node0] /' &
PID0=$!

# Wait for node 0 to initialize
sleep 2

if ! kill -0 $PID0 2>/dev/null; then
    echo "ERROR: Node 0 failed to start"
    exit 1
fi
echo "Node 0 started (PID=$PID0)"

# Start Node 1 (joins cluster)
echo ""
echo "--- Starting Node 1 (joining cluster) ---"
CXL_BASE_ADDR=0x200000000 $SERVER \
    --comm-mode distributed \
    --node-id 1 \
    --dist-shm-name "$DIST_SHM" \
    --transport-mode shm \
    --capacity $CAPACITY \
    --port 9991 \
    2>&1 | sed 's/^/[node1] /' &
PID1=$!

# Wait for node 1 to initialize
sleep 2

if ! kill -0 $PID1 2>/dev/null; then
    echo "ERROR: Node 1 failed to start"
    exit 1
fi
echo "Node 1 started (PID=$PID1)"

echo ""
echo "=== Both nodes running ==="
echo "  Node 0: PID=$PID0 (base=0x100000000)"
echo "  Node 1: PID=$PID1 (base=0x200000000)"
echo ""
echo "SHM segments:"
ls -la /dev/shm/cxlmemsim_dist2node* 2>/dev/null || echo "  (none found)"
echo ""
echo "Press Ctrl+C to stop both servers..."

# Wait for either to exit
wait -n $PID0 $PID1
echo "A node exited. Shutting down."
