# CXLMemSim and MEMU

# CXLMemSim
The CXL.mem simulator uses the target latency for simulating the CPU perspective taking ROB and different cacheline states into penalty from the application level.

## Prerequisite
```bash
root@victoryang00-ASUS-Zenbook-S-14-UX5406SA-UX5406SA:/home/victoryang00/CLionProjects/CXLMemSim-dev/build# uname -a
Linux victoryang00-ASUS-Zenbook-S-14-UX5406SA-UX5406SA 6.13.0-rc4+ #12 SMP PREEMPT_DYNAMIC Fri Jan 24 07:08:46 CST 2025 x86_64 x86_64 x86_64 GNU/Linux
```
## User input
```bash
SPDLOG_LEVEL=debug ./CXLMemSim -t ./microbench/ld -i 5 -c 0,2 -d 85 -c 100,100 -w 85.5,86.5,87.5,85.5,86.5,87.5,88. -o "(1,(2,3))"
```
1. -t Target: The path to the executable
2. -i Interval: The epoch of the simulator, the parameter is in milisecond
3. -c CPUSet: The core id to run the executable and the rest will be `setaffinity` to one other core
4. -d Dram Latency: The current platform's DRAM latency, default is 85ns # mark that bw in the remote
5. -b, -l Bandwidth, Latency: Both use 2 input in the vector, first for read, second for write
6. -c Capacity: The capacity of the memory with first be local, remaining accordingly to the input vector.
7. -w Weight: Use the heuristic to calculate the bandwidth
8. -o Topology: Construct the topology using newick tree syntax (1,(2,3)) stands for 
```bash
            1
          /
0 - local
          \
                   2
         switch  / 
                 \ 
                  3
```
9. env SPDLOG_LEVEL stands for logs level that you can see.

## Cite
```bash
@article{yangyarch23,
  title={CXLMemSim: A pure software simulated CXL.mem for performance characterization},
  author={Yiwei Yang, Pooneh Safayenikoo, Jiacheng Ma, Tanvir Ahmed Khan, Andrew Quinn},
  journal={arXiv preprint arXiv:2303.06153},
  booktitle={The fifth Young Architect Workshop (YArch'23)},
  year={2023}
}
```

# MEMU

Compute Express Link (CXL) 3.0 introduces powerful memory pooling and promises to transform datacenter architectures. However, the lack of available CXL 3.0 hardware and the complexity of multi-host configurations pose significant challenges to the community. This paper presents MEMU, a comprehensive emulation framework that enables full CXL 3.0 functionality, including multi-host memory sharing and pooling support. MEMU provides emulation of CXL 3.0 features—such as fabric management, dynamic memory allocation, and coherent memory sharing across multiple hosts—in advance of real hardware availability. An evaluation of MEMU shows that it achieves performance within about 3x of projected native CXL 3.0 speeds having complete compatibility with existing CXL software stacks. We demonstrate the utility of MEMU through a case study on Genomics Pipeline, observing up to a 15% improvement in application performance compared to traditional RDMA-based approaches. MEMU is open-source and publicly available, aiming to accelerate CXL 3.0 research and development.

```bash
sudo ip link add br0 type bridge
sudo ip link set br0 up
sudo ip addr add 192.168.100.1/24 dev br0
for i in 0 1; do
    sudo ip tuntap add tap$i mode tap
    sudo ip link set tap$i up
    sudo ip link set tap$i master br0
done
sudo iptables -t nat -A POSTROUTING -s 192.168.100.0/24 -o eno2 -j MASQUERADE
sudo iptables -A FORWARD -i br0 -o eno2 -j ACCEPT
sudo iptables -A FORWARD -i eno2 -o br0 -m state --state RELATED,ESTABLISHED -j ACCEPT
mkdir build
cd build
wget https://asplos.dev/about/qemu.img
wget https://asplos.dev/about/bzImage
cp qemu.img qemu1.img
../qemu_integration/launch_qemu_cxl1.sh
# in qemu
vi /usr/local/bin/*.sh
# change 192.168.100.10 to 11
vi /etc/hostname
# change node0 to node1
exit
# out of qemu
../qemu_integration/launch_qemu_cxl.sh &
../qemu_integration/launch_qemu_cxl1.sh &
```

for multiple hosts, you'll need vxlan

```bash
#!/bin/bash
set -eux

DEV=enp23s0f0np0
BR=br0
VNI=100
MCAST=239.1.1.1
BR_IP_SUFFIX=$(hostname | grep -oE '[0-9]+$' || echo 1)   # optional auto-index
# Or set manually:
# BR_IP_SUFFIX=<1..4>

# Clean up
ip link del $BR 2>/dev/null || true
ip link del vxlan$VNI 2>/dev/null || true

# Create bridge
ip link add $BR type bridge
ip link set $BR up

# Create multicast VXLAN (no remote attribute!)
ip link add vxlan$VNI type vxlan id $VNI group $MCAST dev $DEV dstport 4789 ttl 10
ip link set vxlan$VNI up
ip link set vxlan$VNI master $BR

# Assign overlay IP
ip addr add 192.168.100.$BR_IP_SUFFIX/24 dev $BR

# Optional: add local TAPs for QEMU
for i in 0 1; do
    ip tuntap add tap$i mode tap
    ip link set tap$i up
    ip link set tap$i master $BR
done

echo "Bridge $BR ready on host $(hostname)"
```
for every host and edit the qemu's ip with `/usr/local/bin/setup*` and `/etc/hostname`.

# CXL Type 2 GPU Emulation

This module enables GPU compute through CXL Type 2 device emulation, allowing a guest VM to access the host's NVIDIA GPU via the CXL.cache coherency protocol.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              GUEST VM                                        │
│  ┌─────────────────┐    ┌──────────────────────────────────────────────┐    │
│  │  CUDA Application │    │           Guest libcuda.so Shim              │    │
│  │  (cuda_test.c)   │───▶│  - Intercepts CUDA Driver API calls          │    │
│  │                  │    │  - Translates to CXL GPU command protocol    │    │
│  └─────────────────┘    │  - Maps BAR2 via /sys/bus/pci/.../resource2  │    │
│                          └──────────────────┬───────────────────────────┘    │
│                                             │ MMIO Read/Write                │
│  ┌──────────────────────────────────────────▼───────────────────────────┐    │
│  │                    CXL Type 2 PCI Device                              │    │
│  │                    Vendor: 0x8086  Device: 0x0d92                     │    │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  │    │
│  │  │    BAR0     │  │    BAR2     │  │    BAR4     │  │    BAR6     │  │    │
│  │  │  Component  │  │ Cache/GPU   │  │   Device    │  │   MSI-X     │  │    │
│  │  │  Registers  │  │  Command    │  │   Memory    │  │             │  │    │
│  │  └─────────────┘  └─────────────┘  └─────────────┘  └─────────────┘  │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                             │                                 │
│  ┌──────────────────────────────────────────▼───────────────────────────┐    │
│  │              Linux Kernel: cxl_type2_accel.ko                         │    │
│  │  - Binds to CXL Type 2 PCI device                                     │    │
│  │  - Configures CXL.cache and CXL.mem capabilities                      │    │
│  │  - Manages cache coherency state                                      │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                              PCIe/CXL Bus
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                              QEMU HOST                                       │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                    CXL Type 2 Device Emulation                        │   │
│  │                    (hw/cxl/cxl_type2.c)                               │   │
│  │                                                                       │   │
│  │  ┌─────────────────────────────────────────────────────────────────┐ │   │
│  │  │                   GPU Command Interface                          │ │   │
│  │  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐   │ │   │
│  │  │  │ Command Regs │  │ Result Regs  │  │    Data Buffer       │   │ │   │
│  │  │  │ 0x00-0x3F    │  │ 0x80-0x9F    │  │    0x1000-0xFFFF     │   │ │   │
│  │  │  │              │  │              │  │    (60KB for PTX,    │   │ │   │
│  │  │  │ CMD, PARAMS  │  │ RESULT0-3    │  │     memcpy data)     │   │ │   │
│  │  │  └──────────────┘  └──────────────┘  └──────────────────────┘   │ │   │
│  │  └────────────────────────────┬────────────────────────────────────┘ │   │
│  │                               │                                       │   │
│  │  ┌────────────────────────────▼────────────────────────────────────┐ │   │
│  │  │                   Coherency Engine                               │ │   │
│  │  │  - Cache line tracking (MESI-like states)                        │ │   │
│  │  │  - Snoop request handling                                        │ │   │
│  │  │  - Writeback management                                          │ │   │
│  │  └────────────────────────────┬────────────────────────────────────┘ │   │
│  │                               │                                       │   │
│  │  ┌────────────────────────────▼────────────────────────────────────┐ │   │
│  │  │                   hetGPU Backend                                 │ │   │
│  │  │                   (hw/cxl/cxl_hetgpu.c)                          │ │   │
│  │  │  - Loads libcuda.so via dlopen()                                 │ │   │
│  │  │  - Translates commands to real CUDA API calls                    │ │   │
│  │  │  - Manages GPU context, memory, kernel launches                  │ │   │
│  │  └────────────────────────────┬────────────────────────────────────┘ │   │
│  └───────────────────────────────│────────────────────────────────────┘   │
│                                  │ dlsym() calls                          │
│  ┌───────────────────────────────▼────────────────────────────────────┐   │
│  │              /usr/lib/x86_64-linux-gnu/libcuda.so                   │   │
│  │              (NVIDIA CUDA Driver Library)                           │   │
│  │  cuInit, cuCtxCreate, cuMemAlloc, cuLaunchKernel, ...               │   │
│  └───────────────────────────────┬────────────────────────────────────┘   │
│                                  │                                         │
│  ┌───────────────────────────────▼────────────────────────────────────┐   │
│  │                    NVIDIA GPU Hardware                              │   │
│  │                    (e.g., RTX 3090, A100)                           │   │
│  └────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

## GPU Command Protocol

The guest communicates with the CXL Type 2 device via MMIO registers in BAR2:

| Offset | Register | Description |
|--------|----------|-------------|
| 0x0000 | MAGIC | Magic number: 0x43584C32 ("CXL2") |
| 0x0004 | VERSION | Interface version |
| 0x0008 | STATUS | Device status (READY, BUSY, ERROR) |
| 0x0010 | CMD | Command register - write triggers execution |
| 0x0014 | CMD_STATUS | Command status (IDLE, RUNNING, COMPLETE) |
| 0x0018 | CMD_RESULT | Result/error code |
| 0x0040-0x78 | PARAM0-7 | Command parameters |
| 0x0080-0x98 | RESULT0-3 | Command results |
| 0x0140 | TOTAL_MEM | Total GPU memory |
| 0x1000-0xFFFF | DATA | Data buffer for PTX, memcpy |

## Supported Commands

| Command | Code | Description |
|---------|------|-------------|
| CMD_INIT | 0x01 | Initialize GPU |
| CMD_GET_DEVICE_COUNT | 0x02 | Get number of GPUs |
| CMD_CTX_CREATE | 0x10 | Create CUDA context |
| CMD_CTX_SYNC | 0x12 | Synchronize context |
| CMD_MEM_ALLOC | 0x20 | Allocate device memory |
| CMD_MEM_FREE | 0x21 | Free device memory |
| CMD_MEM_COPY_HTOD | 0x22 | Copy host to device |
| CMD_MEM_COPY_DTOH | 0x23 | Copy device to host |
| CMD_MODULE_LOAD_PTX | 0x30 | Load PTX module |
| CMD_FUNC_GET | 0x32 | Get kernel function |
| CMD_LAUNCH_KERNEL | 0x40 | Launch GPU kernel |

## Data Flow Example: cuMemAlloc

```
Guest                     QEMU CXL Type 2              Host GPU
  │                             │                          │
  │ 1. Write size to PARAM0     │                          │
  │ ──────────────────────────▶ │                          │
  │                             │                          │
  │ 2. Write CMD_MEM_ALLOC      │                          │
  │ ──────────────────────────▶ │                          │
  │                             │ 3. Call cuMemAlloc_v2()  │
  │                             │ ───────────────────────▶ │
  │                             │                          │
  │                             │ 4. Return device pointer │
  │                             │ ◀─────────────────────── │
  │                             │                          │
  │                             │ 5. Store in RESULT0      │
  │ 6. Poll CMD_STATUS          │                          │
  │ ──────────────────────────▶ │                          │
  │                             │                          │
  │ 7. Read RESULT0 (dev ptr)   │                          │
  │ ◀────────────────────────── │                          │
```

## Setup Instructions

### 1. Build QEMU with CXL Type 2 Support
```bash
cd lib/qemu/build
meson setup --reconfigure
ninja
```

### 2. Build Guest libcuda Shim
```bash
cd qemu_integration/guest_libcuda
make
```

### 3. Load Kernel Modules (in Guest)
```bash
modprobe cxl_core
modprobe cxl_port
modprobe cxl_cache
modprobe cxl_type2_accel
```

### 4. Run CUDA Applications (in Guest)
```bash
# Set library path to use the CXL shim instead of real libcuda
LD_LIBRARY_PATH=/path/to/guest_libcuda ./your_cuda_app

# Enable debug logging
CXL_CUDA_DEBUG=1 LD_LIBRARY_PATH=. ./cuda_test
```

## QEMU Command Line Options

```bash
-device cxl-type2,id=cxl-gpu0,\
    cache-size=128M,\           # CXL.cache size
    mem-size=4G,\               # Device-attached memory
    hetgpu-lib=/path/to/libcuda.so,\  # CUDA library path
    hetgpu-device=0             # GPU device index
```

## CXL.cache Coherency

The CXL Type 2 device implements CPU-GPU cache coherency:

```
    CPU Cache                  CXL Type 2 Device
        │                            │
        │  ◀─── Snoop Request ────── │  (GPU wants exclusive access)
        │                            │
        │  ──── Snoop Response ────▶ │  (CPU provides data/invalidates)
        │                            │
        │  ◀─── Writeback ────────── │  (GPU writes back dirty data)
        │                            │
```

This enables:
- Zero-copy data sharing between CPU and GPU
- Coherent memory regions visible to both processors
- Reduced memory copy overhead for GPU compute