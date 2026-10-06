# Zettai RPU v0.2 双 QEMU

Rust OOT 驱动在 `~/zettbridge`，目标内核为 `~/linux`。标准 CXL Type 3 位于 Host A；独立 `zettbridge` 管理 function 位于两端，Host B 的同一 function 负责经 IOMMU 执行 DMA。

驱动源码：[Zettai-US/zettbridge](https://github.com/Zettai-US/zettbridge)，配套提交 `88ea30458c6520bc125fcbb752b5b71f7e26d1e7`。新环境先将驱动克隆到 `~/zettbridge`，并初始化本分支固定的 QEMU 子模块：

```sh
git clone git@github.com:Zettai-US/zettbridge.git ~/zettbridge
git -C ~/zettbridge checkout 88ea30458c6520bc125fcbb752b5b71f7e26d1e7
git submodule update --init lib/qemu
```

```sh
make -C ~/zettbridge KDIR=~/linux LLVM=1
make -C ~/zettbridge tools test
ninja -C lib/qemu/build qemu-system-x86_64 -j 12
./qemu_integration/run_zettbridge_pair.sh --output /tmp/zettbridge-demo
```

脚本实际启动两个 VM，用各自的内核、内存和一次性 initramfs 测试 CXL mmap、用量中继及安全回收，不使用现有磁盘镜像。成功打印 `PAIR PASS`，输出目录保留两端日志及启动命令。

```sh
# ACK 丢失后利用带外查询恢复
./qemu_integration/run_zettbridge_pair.sh --drop-commit-ack --output /tmp/zettbridge-ack
# 结果无法确认时保留 backing / DMA mapping
./qemu_integration/run_zettbridge_pair.sh --drop-commit-ack --query-unknown \
    --output /tmp/zettbridge-quarantine
# Rust FD / PCI 解绑 / 模块卸载生命周期
./qemu_integration/run_zettbridge_pair.sh --lifetime --output /tmp/zettbridge-lifetime
# 管理 ABI 回归，启动两个 qtest QEMU，不依赖 guest 内核
python3 lib/qemu/tests/qtest/zettbridge-test.py lib/qemu/build/qemu-system-x86_64
# ATS 翻译、失效、权限和回收测试
python3 lib/qemu/tests/qtest/zettbridge-ats-test.py lib/qemu/build/qemu-system-x86_64
```

扩大到 8 GiB 并保持运行：

```sh
./qemu_integration/run_zettbridge_pair.sh --ats on --capacity 8G \
    --provider-memory 12G --consumer-memory 4G --keep-running \
    --output /tmp/zettbridge-8g
python3 ~/zettbridge/tools/guest_console.py /tmp/zettbridge-8g consumer \
    --command 'zbctl consumer-stats'
python3 ~/zettbridge/tools/stop_pair.py /tmp/zettbridge-8g
```

`PAIR RUNNING` 表示全 aperture 分窗采样和持续读写已就绪，两个进程会保持运行。串口及 QMP 使用输出目录里的本地 Unix sockets；没有开放网络端口。正式测试覆盖 256 MiB 和 8 GiB，单个 lease 最大 4 GiB，大池通过多个 lease 使用。

可用 `ZETTBRIDGE_DIR`、`KDIR`、`QEMU_BINARY` 覆盖路径。详细 UAPI、unsafe/锁序审计、部署 manifest 和已验证/未验证的 K0–K6 边界在驱动仓库的 `README.md` 与 `docs/`。

当前默认模式为 x86-64、256 MiB / 8 GiB、单路静态 volatile CXL、Host B DRAM、UC lease 映射、IOMMU 和 ATS 持续开启。`--ats off` 可复现原基线。Host B 使用 requester 域内的 ATS 翻译缓存，按 VT-d 的设备失效通知删除条目，RETIRE 清空 ATC 后保持 ATS 进行 DMA unmap/free；活动期间关闭 ATS 会使导出故障。QEMU 的 Unix SEQPACKET 是模型内部互连。同步完成仅是功能验证，不能视为物理 FPGA posted-write drain 或硬件时延验收。

驱动使用目标内核的原生 Rust PCI、MiscDevice、Arc/Mutex、工作队列和 DMA 接口；仅为缺失的内核宏/inline 和私有 DAX ABI 保留无状态 C helper，未修改目标内核。

ATS 的启用由 `~/linux` 的 Intel IOMMU 驱动管理，Rust OOT 驱动检查并持续监测；不启用 PASID/PRI/SVA。Host A 的 lease 用量仍每约 200 ms 中继到 Host B，2 秒未更新即失效。新只读诊断位于 `/sys/class/rpu/rpu0/ats_*`，包括 enabled、requests、hits、invalidations、cached_entries、faults 和 transitions。详细说明见 [驱动 ATS 文档](https://github.com/Zettai-US/zettbridge/blob/main/docs/ats.md)。

边界修复包含 Rust VMA 撤销批次隔离、局部撤销期间收到全局 revoke 的二次 zap、幂等故障处理，以及 QEMU 的 ATS notifier 注册失败、发布前复位和 ATTACH 前断连处理。用量过期后显示 unknown，报告恢复后重新有效；peer 丢失时保留隔离资源，ATS 诊断不再把全 1 MMIO 当成 enabled。验证结果及边界见 [驱动边界测试记录](https://github.com/Zettai-US/zettbridge/blob/main/docs/edge-cases.md)。

```sh
# 13 项硬件模型边界测试
python3 lib/qemu/tests/qtest/zettbridge-edge-test.py lib/qemu/build/qemu-system-x86_64
# 新建专用双 VM：4 线程 128 轮 mmap 回收、报告过期/恢复、两端断连隔离
python3 ~/zettbridge/tools/edge_pair.py --output /tmp/zettbridge-edge-new
```

边界脚本要求新的输出目录；它只关闭本次创建的测试实例。更新源码和构建不会替换已运行 VM 的模型或驱动，常驻 8 GiB pair 保留原二进制。
