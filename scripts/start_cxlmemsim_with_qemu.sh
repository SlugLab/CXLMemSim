#!/bin/bash

echo "=== CXLMemSim with QEMU Integration ==="
echo
echo "Current Configuration:"
echo "- RDMA devices (irdma0, irdma1) are ACTIVE"
echo "- CXLMemSim RDMA server supports TCP fallback"
echo "- QEMU CXL device currently only supports TCP connections"
echo

# Kill any existing servers
pkill -f cxlmemsim_server

echo "Starting CXLMemSim RDMA server (with TCP fallback)..."
echo "Note: RDMA binding may fail, but TCP fallback will work"
echo

# Start the RDMA server with environment variables
export CXL_TRANSPORT_MODE=rdma
export CXL_MEMSIM_RDMA_SERVER=192.168.10.1
export CXL_MEMSIM_RDMA_PORT=5555

# Start server in background
/storage/CXLMemSim/build/cxlmemsim_server_rdma 9999 5555 &
SERVER_PID=$!

echo "Server started with PID: $SERVER_PID"
echo "TCP port: 9999 (for QEMU connection)"
echo "RDMA port: 5555 (attempted, may fallback to TCP)"
echo

echo "To start QEMU with CXL support:"
echo "  cd /storage/CXLMemSim/qemu_integration"
echo "  ./launch_qemu_cxl.sh  # or ./launch_qemu_cxl1.sh"
echo
echo "QEMU will connect to CXLMemSim server on TCP port 9999"
echo
echo "To stop the server:"
echo "  kill $SERVER_PID"