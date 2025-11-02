#!/bin/bash
# 重启两个VM使用共享内存

echo "========================================="
echo "重启VM以使用共享内存配置"
echo "========================================="
echo ""

# 找到QEMU进程并杀掉
echo "[1/5] 停止现有VM..."
pkill -9 qemu-system-x86_64
sleep 3

# 确保cxlmemsim_server正在运行
echo "[2/5] 检查cxlmemsim_server..."
if ! pgrep -x cxlmemsim_server > /dev/null; then
    echo "  错误: cxlmemsim_server未运行"
    echo "  请先启动server"
    exit 1
fi
echo "  ✓ Server正在运行"

# 检查共享内存文件
echo "[3/5] 检查共享内存文件..."
if [ ! -f /dev/shm/cxlmemsim_shared ]; then
    echo "  错误: /dev/shm/cxlmemsim_shared 不存在"
    exit 1
fi
echo "  ✓ 共享内存文件存在: $(ls -lh /dev/shm/cxlmemsim_shared | awk '{print $5}')"

# 创建LSA文件（如果不存在）
echo "[4/5] 准备LSA文件..."
for i in 0 1; do
    LSA_FILE="/tmp/lsa${i}.raw"
    if [ ! -f "$LSA_FILE" ]; then
        echo "  创建 $LSA_FILE..."
        truncate -s 256M "$LSA_FILE"
    fi
done
echo "  ✓ LSA文件准备完成"

# 启动VM
echo "[5/5] 启动VM..."
echo ""
echo "启动Node 0..."
cd /home/yhgan913/CXLMemSim/qemu_integration
nohup ./launch_qemu_cxl.sh > /tmp/qemu0.log 2>&1 &
QEMU0_PID=$!
echo "  PID: $QEMU0_PID"

sleep 5

echo "启动Node 1..."
nohup ./launch_qemu_cxl1.sh > /tmp/qemu1.log 2>&1 &
QEMU1_PID=$!
echo "  PID: $QEMU1_PID"

echo ""
echo "========================================="
echo "VM已启动"
echo "========================================="
echo "Node 0 PID: $QEMU0_PID"
echo "Node 1 PID: $QEMU1_PID"
echo ""
echo "查看日志:"
echo "  tail -f /tmp/qemu0.log"
echo "  tail -f /tmp/qemu1.log"
echo ""
echo "等待10秒让VM启动..."
sleep 10

echo "检查VM状态..."
ping -c 1 -W 2 192.168.100.10 > /dev/null && echo "  ✓ Node 0 (192.168.100.10) 可达" || echo "  ✗ Node 0 不可达"
ping -c 1 -W 2 192.168.100.11 > /dev/null && echo "  ✓ Node 1 (192.168.100.11) 可达" || echo "  ✗ Node 1 不可达"
