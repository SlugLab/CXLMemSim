# CXLMemSim

CXLMemSim is a software framework for studying CXL memory systems without requiring complete CXL hardware. The repository contains two related pieces:

- a CXL memory latency, bandwidth, topology, and coherency simulator.
- a QEMU-integrated CXL emulation stack for Type 3 memory devices, distributed memory pooling, and experimental Type 2 accelerator/GPU support.

The implementation is intended for full-system experiments where guest software talks to a realistic CXL device interface while the host side records and controls protocol-level behavior such as latency, routing, coherency state, directory pressure, and memory placement.

## Repository Layout

Important implementation paths:

```text
CMakeLists.txt                         Top-level C++20 build
include/                               Public headers for simulator/server code
src/                                   CXLMemSim server, controller, coherency, HDM decode
microbench/                            Microbenchmarks
tests/                                 Server/distributed-mode tests
qemu_integration/                      Launch scripts and guest-side integration
qemu_integration/guest_libcuda/        Guest CUDA Driver API shim for CXL Type 2 GPU
lib/qemu/                              QEMU tree with CXL Type 2/Type 3 device changes
lib/qemu/hw/cxl/cxl_type2.c            QEMU CXL Type 2 device model
lib/qemu/hw/cxl/cxl_hetgpu.c           Host GPU backend bridge
lib/qemu/include/hw/cxl/               QEMU CXL Type 2 protocol headers
```

## Implementation Overview

At a high level, the stack has four layers:

```text
Guest applications
  -> guest driver/runtime shim
  -> QEMU CXL device model
  -> CXLMemSim server and host backends
  -> host memory, shared memory, RDMA/TCP transport, or host GPU
```

For Type 3 memory experiments, QEMU forwards memory operations to `cxlmemsim_server`. The server owns the simulated memory pool, applies latency and topology policies, and tracks coherency metadata.

For Type 2 accelerator experiments, the guest uses a CUDA-compatible shim library. CUDA Driver API calls are translated into MMIO commands on a QEMU CXL Type 2 PCI device. QEMU then forwards those commands to the host-side hetGPU backend, which can call the real NVIDIA CUDA driver through `dlopen()` and `dlsym()`.

## CXLMemSim Core

The original CXLMemSim path models CXL.mem behavior from the CPU perspective. It accounts for target DRAM latency, CXL fabric latency, bandwidth, topology, ROB effects, and cache-line states when estimating application-visible memory penalty.

The controller implementation is centered on:

- `CXLController`, which owns the topology, endpoints, policies, and latency model.
- `CXLMemExpander`, which represents CXL-attached memory capacity with separate read/write bandwidth and latency.
- Allocation, migration, paging, and caching policies used by the controller.
- Newick-style topology parsing, for example `(1,(2,3));`.
- An LRU cache and last-branch-record based accounting path for application memory behavior.

Example topology:

```text
          endpoint 1
        /
host -- switch -- endpoint 2
        \
          endpoint 3
```

The topology can be expressed as:

```text
(1,(2,3));
```

## CXLMemSim Server Stack

CXLMemSim adds a server-oriented CXL memory backend. The top-level build always enables `SERVER_MODE` and builds:

- `cxlmemsim`, a static library with the core simulator.
- `cxlmemsim_server`, the Type 3 memory server.
- `cxlmemsim_latency`, a latency calculator.
- `test_distributed_shm`, a distributed shared-memory test.

The server entry point is `src/main_server.cc`. It creates a CXL controller, adds a CXL memory expander endpoint, loads the topology, initializes a shared memory pool, and then serves requests from QEMU or test clients.

Supported request classes include:

- cache-line reads and writes,
- shared-memory information queries,
- atomic fetch-and-add,
- atomic compare-and-swap,
- memory fences,
- Label Storage Area reads and writes.

The server supports several communication modes:

| Mode | Purpose |
| --- | --- |
| `tcp` | Socket-based QEMU/server communication. |
| `shm` | Shared-memory ring-buffer communication through `/dev/shm`. |
| `pgas-shm` | PGAS-style shared memory protocol used by `cxl_backend.h` clients. |
| `distributed` | Multi-node memory server mode with SHM, TCP, RDMA, or hybrid transport. |

The memory pool is managed by `SharedMemoryManager`. It can use POSIX shared memory or a regular file as a backing store. The shared-memory header records a magic value, format version, total size, data offset, base address, and cache-line count. The default cache-line data area is mapped with `mmap()` and is reused when the backing object already exists.

## Coherency and Distributed Memory

The distributed path is implemented in:

- `src/distributed_server.cpp`
- `src/coherency_engine.cpp`
- `src/hdm_decoder.cpp`
- TCP and RDMA communication modules

The HDM decoder supports:

- range-based address decode,
- interleaved address decode,
- hybrid decode that tries explicit ranges before falling back to interleaving.

The coherency engine maintains a directory entry per cache line and models a MOESI-like state machine. Directory entries track:

- cache-line address,
- state,
- owner node,
- owner head,
- sharer set,
- version,
- dirty-data status,
- last access timestamp.

Reads and writes update this directory, calculate coherency-message latency, track remote operations, and account for invalidations, writebacks, ownership transfers, and contention between active heads. Distributed mode can use shared memory, TCP, RDMA, or hybrid transport. TCP and RDMA modes support LogP-style calibration for remote message latency.

## Type 2 GPU Device Stack

The Type 2 GPU path emulates a CXL Type 2 accelerator device that combines:

- a CXL.cache-style coherent request path,
- a CXL.mem-style device-memory aperture,
- MMIO command registers for accelerator operations,
- an optional host GPU backend through CUDA,
- optional VFIO-oriented passthrough helpers.

The main implementation files are:

```text
lib/qemu/hw/cxl/cxl_type2.c
lib/qemu/hw/cxl/cxl_hetgpu.c
lib/qemu/include/hw/cxl/cxl_type2_gpu_cmd.h
qemu_integration/guest_libcuda/libcuda.c
qemu_integration/guest_libcuda/cxl_gpu_cmd.h
```

The guest sees a PCI device with vendor ID `0x8086` and device ID `0x0d92`. The guest CUDA shim scans `/sys/bus/pci/devices`, finds this device, enables it, maps BAR2 through `resource2`, verifies the `CXL2` magic value, and then uses MMIO reads and writes to issue GPU commands.

### Type 2 Data Path

```text
CUDA application in guest
  -> guest libcuda.so shim
  -> BAR2 MMIO command registers
  -> QEMU cxl-type2 device
  -> hetGPU backend
  -> host libcuda.so
  -> physical NVIDIA GPU
```

For memory operations, the shim chunks large transfers through the BAR2 data buffer. It serializes command sequences with `flock()` so multiple guest processes do not interleave register writes, command execution, and result reads.

### Type 2 MMIO Protocol

The Type 2 GPU command interface uses BAR2 for control and data transfer.

| Offset | Register | Description |
| --- | --- | --- |
| `0x0000` | `MAGIC` | `0x43584c32`, the string `CXL2`. |
| `0x0004` | `VERSION` | Command interface version. |
| `0x0008` | `STATUS` | Ready, busy, error, and context-active bits. |
| `0x000c` | `CAPS` | Bulk transfer, coherent cache, DMA, pool, and bias capabilities. |
| `0x0010` | `CMD` | Command register. Writes trigger execution. |
| `0x0014` | `CMD_STATUS` | Idle, pending, running, complete, or error. |
| `0x0018` | `CMD_RESULT` | CUDA-compatible result or error code. |
| `0x0040-0x0078` | `PARAM0-7` | Command parameters. |
| `0x0080-0x0098` | `RESULT0-3` | Command results. |
| `0x0100` | `DEV_NAME` | Device name. |
| `0x0140` | `TOTAL_MEM` | Total device memory. |
| `0x0148` | `FREE_MEM` | Free device memory. |
| `0x1000` | `DATA` | 1 MB transfer buffer for PTX, memcpy data, and arguments. |

BAR4 is reserved for larger bulk transfer experiments with a 64 MB transfer region.

### Supported Type 2 Commands

The command protocol includes:

- device initialization and device-property queries,
- context create, destroy, and synchronize,
- memory allocate, free, copy, set, and memory-info operations,
- PTX module load and function lookup,
- kernel launch,
- stream and event operations,
- bulk transfer commands,
- cache flush, invalidate, and writeback commands,
- Type 2 to Type 3 peer-to-peer DMA discovery and transfer commands,
- coherent shared-memory pool commands,
- host/device bias commands,
- coherency statistics commands.

The guest shim implements the CUDA Driver API subset needed by the tests and benchmarks, including `cuInit`, `cuDeviceGetCount`, `cuCtxCreate`, `cuMemAlloc`, `cuMemcpyHtoD`, `cuMemcpyDtoH`, `cuModuleLoadData`, `cuModuleGetFunction`, and `cuLaunchKernel`.

### Host GPU Backend

`cxl_hetgpu.c` loads the host CUDA driver dynamically. By default it tries:

```text
/usr/lib/x86_64-linux-gnu/libcuda.so
/usr/lib64/libcuda.so
libcuda.so.1
```

It resolves CUDA Driver API symbols such as:

- `cuInit`
- `cuDeviceGetCount`
- `cuDeviceGet`
- `cuCtxCreate_v2`
- `cuMemAlloc_v2`
- `cuMemcpyHtoD_v2`
- `cuMemcpyDtoH_v2`
- `cuModuleLoadData`
- `cuModuleGetFunction`
- `cuLaunchKernel`

The backend creates a per-device CUDA context, records device properties, and wraps CUDA calls with a global mutex so multiple CXL Type 2 devices or MIG instances do not race on CUDA context state.

If real GPU initialization fails, the current implementation reports an error instead of silently falling back to simulation for the Type 2 GPU path.

### Type 2 Coherency Model

`cxl_type2.c` maintains cache-line metadata for the emulated device. The model tracks cache lines, dirty state, timestamps, cache hits, cache misses, coherency operations, and snoops.

The device model supports:

- cache-line lookup and insertion,
- invalidation,
- dirty writeback,
- snoop request handling,
- downgrade to shared state,
- writeback to device memory,
- optional notification to CXLMemSim,
- callback integration with the hetGPU backend.

When the GPU writes to a coherent region, the callback invalidates affected cache lines. When the GPU reads, the callback writes back affected CPU-side dirty lines before the host GPU operation proceeds.

## Runtime Stack

The runtime stack depends on the experiment type.

For Type 3 memory experiments:

```text
guest OS / workload
  -> QEMU CXL Type 3 device
  -> TCP, SHM, PGAS-SHM, or distributed transport
  -> cxlmemsim_server
  -> SharedMemoryManager
  -> CXLController, HDM decoder, coherency engine, topology model
```

For Type 2 GPU experiments:

```text
guest CUDA workload
  -> qemu_integration/guest_libcuda/libcuda.so.1
  -> QEMU cxl-type2 PCI device
  -> BAR2 command protocol
  -> cxl_hetgpu backend
  -> host NVIDIA CUDA driver
  -> host GPU
```

For distributed memory experiments:

```text
multiple QEMU guests or server nodes
  -> local cxlmemsim_server instance
  -> distributed message manager
  -> SHM, TCP, RDMA, or hybrid transport
  -> remote memory server
  -> distributed coherency engine
```

## Build

The project uses CMake and C++20. It depends on `cxxopts` and header-only `spdlog`. RDMA support is enabled when `librdmacm` and `libibverbs` are found.

```bash
mkdir -p build
cd build
cmake ..
cmake --build . -j
```

Build the QEMU tree with the CXL Type 2 support:

```bash
cd lib/qemu/build
meson setup --reconfigure
ninja
```

Build the guest CUDA shim:

```bash
cd qemu_integration/guest_libcuda
make
```

The shim build creates:

```text
libcuda.so.1
libcuda.so
libnvcuda.so.1
libnvcuda.so
```

## Running CXLMemSim Server

Basic Type 3 server:

```bash
./build/cxlmemsim_server \
  --comm-mode=tcp \
  --port=9999 \
  --capacity=256 \
  --default_latency=100 \
  --topology=topology.txt
```

Shared-memory mode:

```bash
./build/cxlmemsim_server \
  --comm-mode=shm \
  --capacity=256
```

PGAS shared-memory mode:

```bash
./build/cxlmemsim_server \
  --comm-mode=pgas-shm \
  --pgas-shm-name=/cxlmemsim_pgas \
  --capacity=256
```

File-backed memory pool:

```bash
./build/cxlmemsim_server \
  --comm-mode=tcp \
  --backing-file=/tmp/cxlmemsim.backing \
  --capacity=1024
```

Useful options:

| Option | Meaning |
| --- | --- |
| `--capacity` | CXL expander capacity in MB. |
| `--default_latency` | Base device latency in ns. |
| `--interleave_size` | Interleave granularity in bytes. |
| `--topology` | Topology file using Newick-style syntax. |
| `--comm-mode` | `tcp`, `shm`, `pgas-shm`, or `distributed`. |
| `--backing-file` | Use a regular file instead of POSIX shared memory. |
| `SPDLOG_LEVEL` | Runtime log level, for example `debug` or `trace`. |

## Distributed Mode

Coordinator node:

```bash
./build/cxlmemsim_server \
  --comm-mode=distributed \
  --node-id=0 \
  --dist-shm-name=/cxlmemsim_dist \
  --capacity=256
```

Second node joining the cluster:

```bash
./build/cxlmemsim_server \
  --comm-mode=distributed \
  --node-id=1 \
  --dist-shm-name=/cxlmemsim_dist \
  --coordinator-shm=/cxlmemsim_dist \
  --capacity=256
```

TCP transport example:

```bash
./build/cxlmemsim_server \
  --comm-mode=distributed \
  --node-id=0 \
  --transport-mode=tcp \
  --tcp-addr=0.0.0.0 \
  --tcp-port=5555 \
  --tcp-peers=1:192.168.100.11:5555 \
  --capacity=256
```

RDMA transport uses the same peer format and uses `--transport-mode=rdma`. The implementation uses TCP port plus 1000 as the RDMA port convention.

## QEMU Network Setup for Multi-Guest Experiments

For two local guests, create a Linux bridge and TAP devices:

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
```

For multiple physical hosts, use a VXLAN-backed bridge on each host:

```bash
DEV=enp23s0f0np0
BR=br0
VNI=100
MCAST=239.1.1.1
BR_IP_SUFFIX=$(hostname | grep -oE '[0-9]+$' || echo 1)

sudo ip link del $BR 2>/dev/null || true
sudo ip link del vxlan$VNI 2>/dev/null || true

sudo ip link add $BR type bridge
sudo ip link set $BR up

sudo ip link add vxlan$VNI type vxlan id $VNI group $MCAST dev $DEV dstport 4789 ttl 10
sudo ip link set vxlan$VNI up
sudo ip link set vxlan$VNI master $BR

sudo ip addr add 192.168.100.$BR_IP_SUFFIX/24 dev $BR

for i in 0 1; do
    sudo ip tuntap add tap$i mode tap
    sudo ip link set tap$i up
    sudo ip link set tap$i master $BR
done
```

Inside each guest, update the guest IP setup scripts under `/usr/local/bin/` and set a unique hostname in `/etc/hostname`.

## Running Type 2 GPU Experiments

Start QEMU with a CXL Type 2 device:

```bash
-device cxl-type2,id=cxl-gpu0,\
    cache-size=128M,\
    mem-size=4G,\
    hetgpu-lib=/usr/lib/x86_64-linux-gnu/libcuda.so,\
    hetgpu-device=0
```

Inside the guest, use the CXL CUDA shim:

```bash
cd qemu_integration/guest_libcuda
make

LD_LIBRARY_PATH=. ./cuda_test
CXL_CUDA_DEBUG=1 LD_LIBRARY_PATH=. ./cuda_test
```

For an existing CUDA Driver API program:

```bash
LD_PRELOAD=./libcuda.so.1 ./your_cuda_program
```

Build all guest-side tests:

```bash
cd qemu_integration/guest_libcuda
make all_tests
```

Available tests include:

- `cuda_test`
- `cuda_advanced_test`
- `gpu_benchmark`
- `coherency_test`
- `p2p_test`
- `cxl_coherent_test`
- `cxl_pointer_sharing_test`
- `cxl_bias_benchmark`

## Legacy Application-Level Simulator Invocation

The original CXLMemSim application-level invocation accepts a target executable, sampling interval, CPU set, DRAM latency, bandwidth/latency vectors, capacity vectors, heuristic weights, and topology.

Example:

```bash
SPDLOG_LEVEL=debug ./CXLMemSim \
  -t ./microbench/ld \
  -i 5 \
  -c 0,2 \
  -d 85 \
  -b 100,100 \
  -w 85.5,86.5,87.5,85.5,86.5,87.5,88 \
  -o "(1,(2,3))"
```

Common options:

| Option | Meaning |
| --- | --- |
| `-t` | Target executable. |
| `-i` | Simulator epoch or interval in milliseconds. |
| `-c` | CPU set used to run the target and pin remaining work. |
| `-d` | Platform DRAM latency in ns. |
| `-b` | Read/write bandwidth vector. |
| `-l` | Read/write latency vector. |
| `-w` | Heuristic weights for bandwidth and latency calculations. |
| `-o` | Newick-style CXL topology. |

## Notes and Limitations

- The Type 2 GPU path is experimental and depends on the modified QEMU tree in `lib/qemu`.
- The guest CUDA shim implements a CUDA Driver API subset, not the full CUDA runtime API.
- Type 2 real-GPU mode requires a working NVIDIA driver and an accessible `libcuda.so` on the host.
- Distributed mode can use SHM, TCP, RDMA, or hybrid transport, but the exact deployment depends on host network and RDMA configuration.
- The emulator exposes protocol counters and controllable knobs for experiments; it should not be treated as a cycle-accurate hardware implementation.

## Citation

```bibtex
@article{yanghpdc26,
  title={CXLMemSim: A pure software simulated CXL.mem for performance characterization},
  author={Yiwei Yang, Pooneh Safayenikoo, Jiacheng Ma, Tanvir Ahmed Khan, Andrew Quinn},
  journal={arXiv preprint arXiv:2303.06153},
  booktitle={The fifth Young Architect Workshop (YArch'23)},
  year={2023}
}
```
