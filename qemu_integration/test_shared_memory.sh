#!/bin/bash
# Test whether shared memory is working

echo "========================================="
echo "Testing shared-memory configuration"
echo "========================================="
echo ""

# 1. Check host shared memory
echo "[1/6] Checking host shared-memory file..."
if [ ! -f /dev/shm/cxlmemsim_shared ]; then
    echo "  ERROR: /dev/shm/cxlmemsim_shared does not exist"
    exit 1
fi
echo "  OK: file exists: $(ls -lh /dev/shm/cxlmemsim_shared | awk '{print $5}')"

# 2. Check which processes are using it
echo "[2/6] Checking processes using shared memory..."
USERS=$(lsof /dev/shm/cxlmemsim_shared 2>/dev/null | tail -n +2 | awk '{print $1}' | sort -u)
if [ -z "$USERS" ]; then
    echo "  WARN: no process is using shared memory"
else
    echo "  OK: processes using shared memory:"
    echo "$USERS" | while read proc; do
        COUNT=$(lsof /dev/shm/cxlmemsim_shared 2>/dev/null | grep -c "$proc")
        echo "    - $proc (${COUNT} instances)"
    done
fi
echo ""

# 3. Check VM reachability
echo "[3/6] Checking VM network connectivity..."
if ping -c 1 -W 2 192.168.100.10 > /dev/null 2>&1; then
    echo "  OK: Node 0 (192.168.100.10) reachable"
    NODE0_UP=1
else
    echo "  FAIL: Node 0 (192.168.100.10) unreachable"
    NODE0_UP=0
fi

if ping -c 1 -W 2 192.168.100.11 > /dev/null 2>&1; then
    echo "  OK: Node 1 (192.168.100.11) reachable"
    NODE1_UP=1
else
    echo "  FAIL: Node 1 (192.168.100.11) unreachable"
    NODE1_UP=0
fi
echo ""

if [ $NODE0_UP -eq 0 ] || [ $NODE1_UP -eq 0 ]; then
    echo "ERROR: one or more VMs are unreachable; cannot continue"
    exit 1
fi

# 4. Check DAX devices
echo "[4/6] Checking DAX devices inside VMs..."
NODE0_DAX=$(ssh root@192.168.100.10 "ls -l /dev/dax0.0 2>&1" | grep -c "^c")
NODE1_DAX=$(ssh root@192.168.100.11 "ls -l /dev/dax0.0 2>&1" | grep -c "^c")

if [ $NODE0_DAX -eq 1 ]; then
    echo "  OK: Node 0: /dev/dax0.0 exists"
else
    echo "  FAIL: Node 0: /dev/dax0.0 does not exist"
fi

if [ $NODE1_DAX -eq 1 ]; then
    echo "  OK: Node 1: /dev/dax0.0 exists"
else
    echo "  FAIL: Node 1: /dev/dax0.0 does not exist"
fi
echo ""

# 5. Test shared-memory write/read
echo "[5/6] Testing shared-memory write/read..."
TEST_STRING="SHARED_MEMORY_TEST_$(date +%s)"
echo "  Writing test string to Node 0: $TEST_STRING"

# Write on Node 0
ssh root@192.168.100.10 "echo -n '$TEST_STRING' | dd of=/dev/dax0.0 bs=1 seek=1024 2>/dev/null"
sleep 1

# Read on Node 1
RESULT=$(ssh root@192.168.100.11 "dd if=/dev/dax0.0 bs=1 skip=1024 count=${#TEST_STRING} 2>/dev/null")

if [ "$RESULT" = "$TEST_STRING" ]; then
    echo "  OK: Node 1 read data written by Node 0"
    echo "    wrote: $TEST_STRING"
    echo "    read: $RESULT"
    SHARED_WORKS=1
else
    echo "  FAIL: shared memory is not working"
    echo "    wrote: $TEST_STRING"
    echo "    read: $RESULT"
    SHARED_WORKS=0
fi
echo ""

# 6. Test reverse direction (Node 1 writes, Node 0 reads)
echo "[6/6] Testing reverse write/read..."
TEST_STRING2="REVERSE_TEST_$(date +%s)"
echo "  Writing test string to Node 1: $TEST_STRING2"

# Write on Node 1
ssh root@192.168.100.11 "echo -n '$TEST_STRING2' | dd of=/dev/dax0.0 bs=1 seek=2048 2>/dev/null"
sleep 1

# Read on Node 0
RESULT2=$(ssh root@192.168.100.10 "dd if=/dev/dax0.0 bs=1 skip=2048 count=${#TEST_STRING2} 2>/dev/null")

if [ "$RESULT2" = "$TEST_STRING2" ]; then
    echo "  OK: Node 0 read data written by Node 1"
    echo "    wrote: $TEST_STRING2"
    echo "    read: $RESULT2"
    REVERSE_WORKS=1
else
    echo "  FAIL: reverse shared-memory path is not working"
    echo "    wrote: $TEST_STRING2"
    echo "    read: $RESULT2"
    REVERSE_WORKS=0
fi
echo ""

# Summary
echo "========================================="
echo "Test summary"
echo "========================================="
if [ $SHARED_WORKS -eq 1 ] && [ $REVERSE_WORKS -eq 1 ]; then
    echo "OK: shared-memory configuration is correct"
    echo ""
    echo "You can now run the Tigon multi-node test:"
    echo "  cd /home/yhgan913/CXLMemSim/workloads/tigon"
    echo "  export CXL_BACKEND=dax"
    echo "  export CXL_MEMORY_RESOURCE=/dev/dax0.0"
    echo "  ./scripts/run.sh TPCC TwoPLPasha 2 3 mixed 10 15 1 0 1 Clock OnDemand 200000000 1 WriteThrough None 30 10 BLACKHOLE 20000 0 0"
    exit 0
else
    echo "FAIL: shared-memory configuration has a problem"
    echo ""
    echo "Troubleshooting:"
    echo "  1. Confirm both VMs use /dev/shm/cxlmemsim_shared"
    echo "  2. Check lsof /dev/shm/cxlmemsim_shared"
    echo "  3. Review /tmp/qemu0.log and /tmp/qemu1.log"
    echo "  4. Restart VMs: ./restart_vms_shared.sh"
    exit 1
fi
