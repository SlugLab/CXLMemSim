#!/bin/bash
# 测试共享内存是否工作

echo "========================================="
echo "测试共享内存配置"
echo "========================================="
echo ""

# 1. 检查host共享内存
echo "[1/6] 检查host共享内存文件..."
if [ ! -f /dev/shm/cxlmemsim_shared ]; then
    echo "  ✗ 错误: /dev/shm/cxlmemsim_shared 不存在"
    exit 1
fi
echo "  ✓ 文件存在: $(ls -lh /dev/shm/cxlmemsim_shared | awk '{print $5}')"

# 2. 检查哪些进程在使用
echo "[2/6] 检查使用共享内存的进程..."
USERS=$(lsof /dev/shm/cxlmemsim_shared 2>/dev/null | tail -n +2 | awk '{print $1}' | sort -u)
if [ -z "$USERS" ]; then
    echo "  ✗ 警告: 没有进程在使用共享内存"
else
    echo "  ✓ 使用中的进程:"
    echo "$USERS" | while read proc; do
        COUNT=$(lsof /dev/shm/cxlmemsim_shared 2>/dev/null | grep -c "$proc")
        echo "    - $proc (${COUNT}个实例)"
    done
fi
echo ""

# 3. 检查VM可达性
echo "[3/6] 检查VM网络连接..."
if ping -c 1 -W 2 192.168.100.10 > /dev/null 2>&1; then
    echo "  ✓ Node 0 (192.168.100.10) 可达"
    NODE0_UP=1
else
    echo "  ✗ Node 0 (192.168.100.10) 不可达"
    NODE0_UP=0
fi

if ping -c 1 -W 2 192.168.100.11 > /dev/null 2>&1; then
    echo "  ✓ Node 1 (192.168.100.11) 可达"
    NODE1_UP=1
else
    echo "  ✗ Node 1 (192.168.100.11) 不可达"
    NODE1_UP=0
fi
echo ""

if [ $NODE0_UP -eq 0 ] || [ $NODE1_UP -eq 0 ]; then
    echo "错误: 一个或多个VM不可达，无法继续测试"
    exit 1
fi

# 4. 检查DAX设备
echo "[4/6] 检查VM内DAX设备..."
NODE0_DAX=$(ssh root@192.168.100.10 "ls -l /dev/dax0.0 2>&1" | grep -c "^c")
NODE1_DAX=$(ssh root@192.168.100.11 "ls -l /dev/dax0.0 2>&1" | grep -c "^c")

if [ $NODE0_DAX -eq 1 ]; then
    echo "  ✓ Node 0: /dev/dax0.0 存在"
else
    echo "  ✗ Node 0: /dev/dax0.0 不存在"
fi

if [ $NODE1_DAX -eq 1 ]; then
    echo "  ✓ Node 1: /dev/dax0.0 存在"
else
    echo "  ✗ Node 1: /dev/dax0.0 不存在"
fi
echo ""

# 5. 测试共享内存写入/读取
echo "[5/6] 测试共享内存读写..."
TEST_STRING="SHARED_MEMORY_TEST_$(date +%s)"
echo "  写入测试字符串到Node 0: $TEST_STRING"

# 在Node 0写入
ssh root@192.168.100.10 "echo -n '$TEST_STRING' | dd of=/dev/dax0.0 bs=1 seek=1024 2>/dev/null"
sleep 1

# 在Node 1读取
RESULT=$(ssh root@192.168.100.11 "dd if=/dev/dax0.0 bs=1 skip=1024 count=${#TEST_STRING} 2>/dev/null")

if [ "$RESULT" = "$TEST_STRING" ]; then
    echo "  ✓ 成功！Node 1 读取到Node 0写入的数据"
    echo "    写入: $TEST_STRING"
    echo "    读取: $RESULT"
    SHARED_WORKS=1
else
    echo "  ✗ 失败！共享内存不工作"
    echo "    写入: $TEST_STRING"
    echo "    读取: $RESULT"
    SHARED_WORKS=0
fi
echo ""

# 6. 测试反向（Node 1写，Node 0读）
echo "[6/6] 测试反向读写..."
TEST_STRING2="REVERSE_TEST_$(date +%s)"
echo "  写入测试字符串到Node 1: $TEST_STRING2"

# 在Node 1写入
ssh root@192.168.100.11 "echo -n '$TEST_STRING2' | dd of=/dev/dax0.0 bs=1 seek=2048 2>/dev/null"
sleep 1

# 在Node 0读取
RESULT2=$(ssh root@192.168.100.10 "dd if=/dev/dax0.0 bs=1 skip=2048 count=${#TEST_STRING2} 2>/dev/null")

if [ "$RESULT2" = "$TEST_STRING2" ]; then
    echo "  ✓ 成功！Node 0 读取到Node 1写入的数据"
    echo "    写入: $TEST_STRING2"
    echo "    读取: $RESULT2"
    REVERSE_WORKS=1
else
    echo "  ✗ 失败！反向共享不工作"
    echo "    写入: $TEST_STRING2"
    echo "    读取: $RESULT2"
    REVERSE_WORKS=0
fi
echo ""

# 总结
echo "========================================="
echo "测试总结"
echo "========================================="
if [ $SHARED_WORKS -eq 1 ] && [ $REVERSE_WORKS -eq 1 ]; then
    echo "✓ 共享内存配置正确！"
    echo ""
    echo "可以运行Tigon多节点测试了："
    echo "  cd /home/yhgan913/CXLMemSim/workloads/tigon"
    echo "  export CXL_BACKEND=dax"
    echo "  export CXL_MEMORY_RESOURCE=/dev/dax0.0"
    echo "  ./scripts/run.sh TPCC TwoPLPasha 2 3 mixed 10 15 1 0 1 Clock OnDemand 200000000 1 WriteThrough None 30 10 BLACKHOLE 20000 0 0"
    exit 0
else
    echo "✗ 共享内存配置有问题"
    echo ""
    echo "故障排查："
    echo "  1. 确认两个VM都使用 /dev/shm/cxlmemsim_shared"
    echo "  2. 检查 lsof /dev/shm/cxlmemsim_shared"
    echo "  3. 查看 /tmp/qemu0.log 和 /tmp/qemu1.log"
    echo "  4. 重启VM: ./restart_vms_shared.sh"
    exit 1
fi
