# Zettai RPU v0.2 双 QEMU

Rust OOT 驱动在 `~/zettbridge`，目标内核为 `~/linux`。标准 CXL Type 3 位于 Host A；独立 `zettbridge` 管理 function 位于两端，Host B 的同一 function 负责经 IOMMU 执行 DMA。

驱动源码：[Zettai-US/zettbridge](https://github.com/Zettai-US/zettbridge)，配套提交 `a2fb553b7dae0a03401c5ab493016890980a7b69`。新环境先将驱动克隆到 `~/zettbridge`，并初始化本分支固定的 QEMU 子模块：

```sh
git clone git@github.com:Zettai-US/zettbridge.git ~/zettbridge
git -C ~/zettbridge checkout a2fb553b7dae0a03401c5ab493016890980a7b69
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
```

扩大到 8 GiB 并保持运行：

```sh
./qemu_integration/run_zettbridge_pair.sh --capacity 8G \
    --provider-memory 12G --consumer-memory 4G --keep-running \
    --output /tmp/zettbridge-8g
python3 ~/zettbridge/tools/guest_console.py /tmp/zettbridge-8g consumer \
    --command 'zbctl consumer-stats'
python3 ~/zettbridge/tools/stop_pair.py /tmp/zettbridge-8g
```

`PAIR RUNNING` 表示全 aperture 分窗采样和持续读写已就绪，两个进程会保持运行。串口及 QMP 使用输出目录里的本地 Unix sockets；没有开放网络端口。正式测试覆盖 256 MiB 和 8 GiB，单个 lease 最大 4 GiB，大池通过多个 lease 使用。

可用 `ZETTBRIDGE_DIR`、`KDIR`、`QEMU_BINARY` 覆盖路径。详细 UAPI、unsafe/锁序审计、部署 manifest 和已验证/未验证的 K0–K6 边界在驱动仓库的 `README.md` 与 `docs/`。

当前基线为 x86-64、256 MiB / 8 GiB、单路静态 volatile CXL、Host B DRAM、UC lease 映射、IOMMU 开 / ATS 关。QEMU 的 Unix SEQPACKET 是设备模型内部互连；每次访存由 Host A 的标准 HDM 转换后，走 Host B 的 `pci_dma_read/write`。不能把本模型的同步 DMA 完成解释为实际 FPGA 的 posted-write drain 或硬件时延验收。

驱动使用目标内核的原生 Rust PCI、MiscDevice、Arc/Mutex、工作队列和 DMA 接口；仅为缺失的内核宏/inline 和私有 DAX ABI 保留无状态 C helper，未修改目标内核。
