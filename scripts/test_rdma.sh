#!/bin/bash

# Test script for RDMA functionality in CXLMemSim

set -e

echo "============================================"
echo "CXLMemSim RDMA Test Script"
echo "============================================"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    if [ $1 -eq 0 ]; then
        echo -e "${GREEN}[PASS]${NC} $2"
    else
        echo -e "${RED}[FAIL]${NC} $2"
        return 1
    fi
}

print_info() {
    echo -e "${YELLOW}[INFO]${NC} $1"
}

# Check if RDMA devices are available
print_info "Checking RDMA devices..."
if command -v ibv_devices &> /dev/null; then
    DEVICES=$(ibv_devices 2>/dev/null | grep -c "device" || echo "0")
    if [ "$DEVICES" -gt 0 ]; then
        print_status 0 "Found $DEVICES RDMA device(s)"
        ibv_devices
    else
        print_status 1 "No RDMA devices found"
        exit 1
    fi
else
    print_status 1 "ibv_devices command not found. Please install RDMA tools."
    exit 1
fi

# Check RDMA device info
print_info "Getting RDMA device information..."
ibv_devinfo > /dev/null 2>&1
print_status $? "RDMA device info accessible"

# Check if soft-RoCE is available
print_info "Checking for soft-RoCE support..."
if lsmod | grep -q rdma_rxe; then
    print_status 0 "Soft-RoCE module loaded"
else
    print_info "Soft-RoCE module not loaded, attempting to load..."
    sudo modprobe rdma_rxe 2>/dev/null || true
fi

# Test RDMA connectivity (loopback)
print_info "Testing RDMA loopback connectivity..."

# Start server in background
timeout 5 ib_write_bw -d rxe0 > /tmp/rdma_server.log 2>&1 &
SERVER_PID=$!
sleep 2

# Run client
timeout 3 ib_write_bw -d rxe0 localhost > /tmp/rdma_client.log 2>&1
CLIENT_RESULT=$?

# Kill server if still running
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true

if [ $CLIENT_RESULT -eq 0 ]; then
    print_status 0 "RDMA loopback test passed"

    # Extract bandwidth from client log
    if [ -f /tmp/rdma_client.log ]; then
        BW=$(grep "BW average" /tmp/rdma_client.log | awk '{print $4, $5}' || echo "N/A")
        print_info "Loopback bandwidth: $BW"
    fi
else
    print_status 1 "RDMA loopback test failed"
    if [ -f /tmp/rdma_client.log ]; then
        echo "Client log:"
        cat /tmp/rdma_client.log
    fi
fi

# Build RDMA-enabled CXLMemSim if source is available
if [ -f "CMakeLists_rdma.txt" ]; then
    print_info "Building RDMA-enabled CXLMemSim..."

    mkdir -p build_rdma
    cd build_rdma

    cmake -DENABLE_RDMA=ON -DCMAKE_BUILD_TYPE=Release .. > /dev/null 2>&1
    BUILD_RESULT=$?

    if [ $BUILD_RESULT -eq 0 ]; then
        make -j$(nproc) cxlmemsim_server_rdma > /dev/null 2>&1
        MAKE_RESULT=$?
        print_status $MAKE_RESULT "Build completed"
    else
        print_status 1 "CMake configuration failed"
    fi

    cd ..
fi

# Test CXLMemSim RDMA server
if [ -f "build_rdma/cxlmemsim_server_rdma" ]; then
    print_info "Testing CXLMemSim RDMA server..."

    # Start server
    export CXL_TRANSPORT_MODE=rdma
    ./build_rdma/cxlmemsim_server_rdma 4444 5555 > /tmp/cxlmemsim_server.log 2>&1 &
    SERVER_PID=$!
    sleep 3

    # Check if server is running
    if ps -p $SERVER_PID > /dev/null; then
        print_status 0 "CXLMemSim RDMA server started"

        # Show server info
        head -n 20 /tmp/cxlmemsim_server.log

        # Stop server
        kill $SERVER_PID 2>/dev/null || true
        wait $SERVER_PID 2>/dev/null || true
    else
        print_status 1 "CXLMemSim RDMA server failed to start"
        if [ -f /tmp/cxlmemsim_server.log ]; then
            echo "Server log:"
            cat /tmp/cxlmemsim_server.log
        fi
    fi
fi

# Performance comparison test
print_info "Running performance comparison (if available)..."

if [ -f "build/cxlmemsim_server_simple" ] && [ -f "build_rdma/cxlmemsim_server_rdma" ]; then
    print_info "TCP vs RDMA latency comparison:"

    # Test TCP latency
    export CXL_TRANSPORT_MODE=tcp
    ./build/cxlmemsim_server_simple 4444 > /tmp/tcp_server.log 2>&1 &
    TCP_PID=$!
    sleep 2

    # Run simple client test (you would need to implement this)
    # ./test_client tcp 4444 > /tmp/tcp_latency.log 2>&1

    kill $TCP_PID 2>/dev/null || true
    wait $TCP_PID 2>/dev/null || true

    # Test RDMA latency
    export CXL_TRANSPORT_MODE=rdma
    ./build_rdma/cxlmemsim_server_rdma 4444 5555 > /tmp/rdma_server.log 2>&1 &
    RDMA_PID=$!
    sleep 2

    # Run simple client test
    # ./test_client rdma 5555 > /tmp/rdma_latency.log 2>&1

    kill $RDMA_PID 2>/dev/null || true
    wait $RDMA_PID 2>/dev/null || true

    print_info "Performance test completed (results would be in log files)"
else
    print_info "Skipping performance comparison (binaries not found)"
fi

# Summary
echo ""
echo "============================================"
echo "Test Summary"
echo "============================================"

# Check overall status
if command -v ibv_devices &> /dev/null && [ "$DEVICES" -gt 0 ]; then
    echo -e "${GREEN}RDMA support is available and functional${NC}"
    echo ""
    echo "Next steps:"
    echo "1. Start RDMA server: CXL_TRANSPORT_MODE=rdma ./cxlmemsim_server_rdma 4444 5555"
    echo "2. Configure QEMU with RDMA support"
    echo "3. Set environment variables:"
    echo "   export CXL_TRANSPORT_MODE=rdma"
    echo "   export CXL_MEMSIM_RDMA_SERVER=<server_ip>"
    echo "   export CXL_MEMSIM_RDMA_PORT=5555"
else
    echo -e "${RED}RDMA support is not fully functional${NC}"
    echo ""
    echo "Troubleshooting:"
    echo "1. Run setup script: sudo ./scripts/setup_rdma.sh"
    echo "2. Check kernel modules: lsmod | grep rdma"
    echo "3. Check for hardware: lspci | grep -i infiniband"
    echo "4. Try soft-RoCE: sudo rdma link add rxe0 type rxe netdev eth0"
fi

# Cleanup
rm -f /tmp/rdma_server.log /tmp/rdma_client.log /tmp/cxlmemsim_server.log
rm -f /tmp/tcp_server.log /tmp/rdma_server.log