#!/bin/bash
# Restart two VMs with shared memory

echo "========================================="
echo "Restart VMs with shared-memory configuration"
echo "========================================="
echo ""

# Find and stop QEMU processes
echo "[1/5] Stopping existing VMs..."
pkill -9 qemu-system-x86_64
sleep 3

# Ensure cxlmemsim_server is running
echo "[2/5] Checking cxlmemsim_server..."
if ! pgrep -x cxlmemsim_server > /dev/null; then
    echo "  ERROR: cxlmemsim_server is not running"
    echo "  Start the server first"
    exit 1
fi
echo "  OK: server is running"

# Check the shared-memory file
echo "[3/5] Checking shared-memory file..."
if [ ! -f /dev/shm/cxlmemsim_shared ]; then
    echo "  ERROR: /dev/shm/cxlmemsim_shared does not exist"
    exit 1
fi
echo "  OK: shared-memory file exists: $(ls -lh /dev/shm/cxlmemsim_shared | awk '{print $5}')"

# Create LSA files if they do not exist
echo "[4/5] Preparing LSA files..."
for i in 0 1; do
    LSA_FILE="/tmp/lsa${i}.raw"
    if [ ! -f "$LSA_FILE" ]; then
        echo "  Creating $LSA_FILE..."
        truncate -s 256M "$LSA_FILE"
    fi
done
echo "  OK: LSA files are ready"

# Start VMs
echo "[5/5] Starting VMs..."
echo ""
echo "Starting Node 0..."
cd /home/yhgan913/CXLMemSim/qemu_integration
nohup ./launch_qemu_cxl.sh > /tmp/qemu0.log 2>&1 &
QEMU0_PID=$!
echo "  PID: $QEMU0_PID"

sleep 5

echo "Starting Node 1..."
nohup ./launch_qemu_cxl1.sh > /tmp/qemu1.log 2>&1 &
QEMU1_PID=$!
echo "  PID: $QEMU1_PID"

echo ""
echo "========================================="
echo "VMs started"
echo "========================================="
echo "Node 0 PID: $QEMU0_PID"
echo "Node 1 PID: $QEMU1_PID"
echo ""
echo "View logs:"
echo "  tail -f /tmp/qemu0.log"
echo "  tail -f /tmp/qemu1.log"
echo ""
echo "Waiting 10 seconds for VM boot..."
sleep 10

echo "Checking VM status..."
ping -c 1 -W 2 192.168.100.10 > /dev/null && echo "  OK: Node 0 (192.168.100.10) reachable" || echo "  FAIL: Node 0 unreachable"
ping -c 1 -W 2 192.168.100.11 > /dev/null && echo "  OK: Node 1 (192.168.100.11) reachable" || echo "  FAIL: Node 1 unreachable"
