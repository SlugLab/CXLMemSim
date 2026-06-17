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
gem5_integration/                      gem5-CXL launch wrapper and O3CPU trace setup
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
- built test binaries for unit and integration checks.
- CTest targets for DCD/GFAM, ROB, and memory-stall unit behavior.

The server entry point is `src/main_server.cc`. It creates a CXL controller, adds a CXL memory expander endpoint, loads the topology, initializes a shared memory pool, and then serves requests from QEMU or test clients.

Supported request classes include:

- cache-line reads and writes,
- shared-memory information queries,
- atomic fetch-and-add,
- atomic compare-and-swap,
- memory fences,
- Label Storage Area reads and writes.
- Dynamic Capacity Device add, release, and query operations.
- GFAM host map, unmap, access-check, and query operations.

The server supports several communication modes:

| Mode | Purpose |
| --- | --- |
| `tcp` | Socket-based QEMU/server communication. |
| `shm` | Shared-memory ring-buffer communication through `/dev/shm`. |
| `pgas-shm` | PGAS-style shared memory protocol used by `cxl_backend.h` clients. |
| `distributed` | Multi-node memory server mode with SHM, TCP, RDMA, or hybrid transport. |

The memory pool is managed by `SharedMemoryManager`. It can use POSIX shared memory or a regular file as a backing store. The shared-memory header records a magic value, format version, total size, data offset, base address, and cache-line count. The default cache-line data area is mapped with `mmap()` and is reused when the backing object already exists.

When `include/ssd_streaming_backend.h` and `src/ssd_streaming_backend.cpp` are present, the server also builds an SSD streaming backing mode. This mode keeps the CXL address space persistent in a backing file or raw block device and routes cache-line reads and writes through `SsdStreamingBackend` instead of exposing a direct mmap pointer. The MVP backend uses 4KB page metadata by default, 64KB read-ahead (`16` pages at 4KB), a resident page cache sized by `--ssd-cache-mb`, CLOCK-like eviction in the backend, Linux `io_uring` with a registered fixed file/buffer when available, O_DIRECT-safe aligned page buffers, and placeholder hint plumbing for prefetch, pin/unpin, evict-after, and streaming ranges.

The server CLI uses local parse-result structs in `src/main_server.cc`, `src/main.cc`, and `src/rob.cc`, so command-line parsing is handled in-tree.

## CPU PMU Compatibility

The application-level simulator uses Linux `perf_event_open()` for sampling. Intel systems use PEBS for load-miss samples and Intel CHA PMUs when the CPU model has a known uncore path. AMD systems use the AMD IBS op PMU when `/sys/bus/event_source/devices/ibs_op/type` is available, then fall back to generic hardware cache-miss sampling if IBS or physical-address sampling is unavailable. Intel CHA counters and LBR accounting are optional on non-Intel systems; unsupported PMU paths are logged and disabled instead of aborting the run.

If monitor setup fails, check the CPU PMU support exposed by the kernel and the perf permission level:

```bash
cat /proc/sys/kernel/perf_event_paranoid
ls /sys/bus/event_source/devices/
```

## Compiler Observability with cxltime SlugAllocator

On macOS, CXLMemSim cannot rely on Linux PEBS/LBR or physical-address PMU sampling. The sibling `cxltime` checkout provides `SlugAllocator`, an LLVM pass plus runtime that instruments basic-block entries and IR memory operations (`load`, `store`, atomics, and memory intrinsics). CXLMemSim can build those tools and run instrumented workloads directly against `cxlmemsim_server`.

Build the SlugAllocator pass/runtime from CXLMemSim:

```bash
cmake -S . -B build \
  -DCXLMEMSIM_ENABLE_SLUGALLOCATOR=ON \
  -DCXLMEMSIM_CXLTIME_ROOT=/Users/yiweiyang/Documents/Lanxin/cxltime
cmake --build build --target slugallocator_tools
```

Compile a workload with the pass:

```bash
./script/cxlmemsim_slug.py compile \
  --cxltime-root /Users/yiweiyang/Documents/Lanxin/cxltime \
  -o app.slug app.c -- -O1 -g
```

Run the CXL server and send Slug memory events to it:

```bash
./build/cxlmemsim_server --comm-mode=tcp --port=9999 --capacity=256

./script/cxlmemsim_slug.py run \
  --trace slug.csv \
  --port 9999 \
  -- ./app.slug
```

Useful runtime variables are `SLUG_TRACE`, `SLUG_CXL_HOST`, `SLUG_CXL_PORT`, `SLUG_REGION_BASE`, `SLUG_REGION_SIZE`, `SLUG_CXL_ADDR_BASE`, and `SLUG_TRACE_ALL=0` for region-only tracing. Without `SLUG_CXL_PORT`, the runtime only records CSV/statistics; with it, every instrumented memory access is split into cacheline-sized read/write requests and sent to CXLMemSim's TCP protocol.

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
- cache flush, invalidate, writeback, and prefetch commands,
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

The project uses CMake and C++20. It depends on header-only `spdlog`; the server and ROB tools use in-tree argument parsing. RDMA support is enabled when `librdmacm` and `libibverbs` are found.

```bash
mkdir -p build
cd build
cmake ..
cmake --build . -j
```

Build the QEMU tree with the CXL Type 2 support:

```bash
./script/build_qemu.sh
```

Build the QEMU tree with vendored hetGPU integrated:

```bash
HETGPU_BUILD=1 ./script/build_qemu.sh
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

## Packaged QEMU/Spack Flow

For macOS and repeatable QEMU smoke tests, use the companion Spack environment from the Ocean Spack fork. That environment builds the x86_64 CXL-capable QEMU tree, builds `cxlmemsim_server`, installs launch scripts, and documents the runtime variables used by QEMU and the server.

```bash
git clone https://github.com/vickiegpt/spack.git ocean-spack
cd ocean-spack
source share/spack/setup-env.sh
spack env activate ./share/spack/environments/cxlmemsim
spack concretize -f
spack install
spack load cxlmemsim
```

The detailed usage document is installed in the Spack checkout at:

```text
share/spack/environments/cxlmemsim/README.md
```

After loading the package, download the guest kernel and disk image:

```bash
cxlmemsim-download-qemu-image
```

The default image directory is `CXL_QEMU_IMAGE_DIR`. To keep the raw guest disk at 4 GB:

```bash
qemu-img resize "$CXL_QEMU_IMAGE_DIR/qemu.img" 4G
cp "$CXL_QEMU_IMAGE_DIR/qemu.img" "$CXL_QEMU_IMAGE_DIR/qemu1.img"
```

Launch the default guest:

```bash
qemu_launch_cxl.sh
```

Launch the second guest image:

```bash
qemu_launch_cxl1.sh
```

The launcher reads the following runtime variables:

| Variable | Default | Meaning |
| --- | --- | --- |
| `CXL_TRANSPORT_MODE` | `shm` | QEMU transport mode: `shm` or `tcp`. |
| `CXL_MEMSIM_HOST` | `127.0.0.1` | Local host for TCP mode. |
| `CXL_MEMSIM_PORT` | `9999` | Local TCP server port. |
| `CXL_PGAS_SHM` | `/cxlmemsim_pgas` | POSIX shared-memory object used by QEMU SHM mode. |
| `CXL_MEMSIM_SERVER_BINARY` | package `bin/cxlmemsim_server` | Server binary started before QEMU. |
| `CXL_MEMSIM_SERVER_AUTOSTART` | `auto` | Set to `1` to require server startup or `0` to disable it. |
| `CXL_DCD_ENABLE` | `0` | Enables DCD reporting in integration wrappers. |
| `CXL_GFAM_ENABLE` | `0` | Enables GFAM reporting in integration wrappers. |
| `CXL_GFAM_HOST_ID` | `0` | Host ID passed to fabric-aware integrations. |
| `CXL_MEMSIM_EARLY_INIT` | `0` | Connect QEMU's CXL Type 3 device to CXLMemSim during device realize instead of waiting for the first memory access. |
| `CXL_MEMSIM_FABRIC_REFRESH_NS` | `1000000000` | Minimum interval between QEMU-side DCD/GFAM query refreshes. Set to `0` to disable periodic refreshes. |

QEMU's `shm` transport uses the PGAS shared-memory protocol, so the packaged launcher maps `CXL_TRANSPORT_MODE=shm` to the server's `--comm-mode pgas-shm`.

Shared-memory launch:

```bash
CXL_TRANSPORT_MODE=shm \
CXL_PGAS_SHM=/cxlmemsim_pgas \
qemu_launch_cxl.sh
```

TCP launch on the local port:

```bash
CXL_TRANSPORT_MODE=tcp \
CXL_MEMSIM_HOST=127.0.0.1 \
CXL_MEMSIM_PORT=9999 \
qemu_launch_cxl.sh
```

When QEMU connects to `cxlmemsim_server`, the Type 3 device queries the DCD and GFAM protocol state. If DCD is enabled, QEMU logs the dynamic allocated, total, and free capacity reported by the server. If GFAM is enabled, QEMU logs host, mapping, operation, denied-access, and average-latency counters. Server-side DCD or GFAM denials are returned to QEMU as memory transaction errors instead of being silently treated as successful reads or writes.

For DCD, QEMU still describes the CXL device topology that the guest sees at boot. The CXLMemSim server owns the dynamic behavior. Keep the QEMU `volatile-dc-memdev` size and `num-dc-regions` aligned with the server `--capacity`, `--dcd-granularity-mb`, and `--dcd-initial-capacity` settings. With `CXL_DCD_ENABLE=1`, QEMU validates the server-reported dynamic capacity during Type 3 initialization and warns if the boot-time CXL layout does not match. Guest DCD accept/release mailbox commands then call the CXLMemSim DCD protocol before QEMU updates its local accepted-extent bitmap.

The Type 3 device also exposes QEMU properties for explicit launch files: `memsim-dcd=on`, `memsim-gfam=on`, and `memsim-gfam-host-id=N`. Environment variables override these properties for existing launch scripts. Existing switch CCI and FM-API DCD commands use the same Type 3 synchronization path; future VCS switching support should call those helpers instead of maintaining a second allocator in QEMU.

For the Zettai RFC VCS switch path, use
`qemu_integration/launch_qemu_vcs_dcd_gfam.sh`. It starts CXLMemSim with
DCD/GFAM enabled, creates a two-USP `zettai` switch, hides the physical DCD
endpoints at boot, and exposes the switch mailbox CCI at `target=zettai0` for
FMAPI bind/unbind testing.

Inside the guest, quick checks are:

```bash
lspci | grep -i cxl
lspci -nn | grep -i '7a74:a123'
dmesg | grep -i cxl
ls /sys/bus/cxl/devices
```

Linux keeps the kernel CXL class names (`mem0`, `port0`, `endpoint0`,
`region0`) even when QEMU uses the `zettai` switch object. To use Zettai names
inside the guest without rebuilding Linux, copy or mount
`qemu_integration/zettai_linux_aliases.sh` into the guest and run:

```bash
sudo ./zettai_linux_aliases.sh
ls -l /run/zettai/cxl
cat /run/zettai/topology.txt
```

For a real kernel-level rename, patch the Linux CXL driver tree under
`drivers/cxl/`; this repository only carries QEMU's copied Linux headers, not a
full Linux kernel source tree.

For a full DCD/GFAM smoke test, launch QEMU with a QMP socket and zero initial
DCD capacity, bind a hidden endpoint from the host, add a DCD extent, then run
the guest region test:

```bash
# host
QEMU_EXTRA_ARGS="-qmp unix:/tmp/zettai-qmp.sock,server=on,wait=off" \
CXL_DCD_INITIAL_CAPACITY_MB=0 \
qemu_integration/launch_qemu_vcs_dcd_gfam.sh

# host, after the guest reaches Linux
python3 qemu_integration/zettai_host_dcd_gfam_test.py --bind --add --query

# guest
sudo ./zettai_guest_dcd_gfam_test.sh

# host, after the guest memory touch
python3 qemu_integration/zettai_host_dcd_gfam_test.py --query
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

Persistent SSD streaming memory pool:

```bash
./build/cxlmemsim_server \
  --comm-mode=tcp \
  --ssd-backing-file=/tmp/cxlmemsim.ssd \
  --ssd-cache-mb=16 \
  --ssd-page-size=4096 \
  --ssd-io-chunk-size=65536 \
  --ssd-read-ahead-pages=16 \
  --ssd-io-uring=true \
  --ssd-odirect=true \
  --capacity=1024
```

SSD streaming is available for TCP, ring-buffer SHM, PGAS SHM request slots, and distributed local-node memory. PGAS and distributed modes keep their control-plane shared memory, but cache-line data is served by `SharedMemoryManager` when SSD streaming is selected.

PGAS shared-memory protocol over SSD streaming:

```bash
./build/cxlmemsim_server \
  --comm-mode=pgas-shm \
  --pgas-shm-name=/cxlmemsim_pgas \
  --ssd-backing-file=/nvme/cxlmemsim-pgas.swap \
  --ssd-cache-mb=16 \
  --capacity=1024
```

In PGAS SSD mode the POSIX shared memory object only carries request/response slots; cache-line data is served from the SSD streaming backend.

Distributed node with SSD streaming local memory:

```bash
./build/cxlmemsim_server \
  --comm-mode=distributed \
  --node-id=0 \
  --dist-shm-name=/cxlmemsim_dist \
  --port=9999 \
  --transport-mode=shm \
  --ssd-backing-file=/nvme/cxlmemsim-node{node}.swap \
  --ssd-cache-mb=16 \
  --capacity=1024
```

For distributed launches, `{node}` in `--ssd-backing-file` is replaced with the node ID. Without the placeholder, the path is used exactly as provided, which is useful when each node process is launched with an explicit raw block device path.

Dynamic Capacity Device and GFAM:

```bash
./build/cxlmemsim_server \
  --comm-mode=tcp \
  --port=9999 \
  --capacity=10000 \
  --default_latency=400 \
  --topology=topology.txt \
  --enable-dcd \
  --dcd-granularity-mb=1 \
  --dcd-initial-capacity=10000 \
  --enable-gfam \
  --gfam-hosts=16 \
  --gfam-fabric-latency=80 \
  --gfam-bandwidth=64
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
| `--ssd-backing-file` | Use persistent SSD streaming storage from a file or raw block device. |
| `--ssd-cache-mb` | Resident SSD backend cache budget in MB. |
| `--ssd-page-size` | SSD backend page size in bytes; default is 4096. |
| `--ssd-io-chunk-size` | SSD backend I/O chunk size in bytes; default is 65536. |
| `--ssd-read-ahead-pages` | Sequential read-ahead window in backend pages; default is 16. |
| `--ssd-io-uring` | Use Linux `io_uring` with registered fixed file and buffer when available; default is on. |
| `--ssd-odirect` | Open the backing file or block device with O_DIRECT when supported; default is on. |
| `--pgas-shm-name` | POSIX shared memory name for PGAS request slots. |
| `--node-id` | Distributed node ID. |
| `--dist-shm-name` | Shared memory name for distributed node queues. |
| `--transport-mode` | Distributed inter-node transport: `shm`, `tcp`, `rdma`, or `hybrid`. |
| `--enable-dcd` | Enable the Dynamic Capacity Device model. |
| `--dcd-granularity-mb` | DCD allocation granularity in MB. |
| `--dcd-initial-capacity` | Initial DCD capacity in MB. Omit it to allocate the full `--capacity`. |
| `--enable-gfam` | Enable GFAM access control and fabric latency accounting. |
| `--gfam-hosts` | Number of GFAM host IDs to register. |
| `--gfam-fabric-latency` | Per-access GFAM fabric latency in ns. |
| `--gfam-bandwidth` | Aggregate GFAM bandwidth in GB/s. |
| `SPDLOG_LEVEL` | Runtime log level, for example `debug` or `trace`. |

DCD/GFAM protocol operations are available over TCP and the PGAS shared-memory protocol. Query responses use the 64-byte response data area: DCD query returns total capacity, free capacity, active extents, and failed requests; GFAM query returns mappings, shared mappings, read/write/atomic operations, denied accesses, and average access latency.

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

For `../llama.cpp-cxl`, build with ggml-cxl enabled and place only the KV cache on
the CXL Type 2 device:

```bash
cmake -S ../llama.cpp-cxl -B ../llama.cpp-cxl/build-cxl -DGGML_CXL=ON
cmake --build ../llama.cpp-cxl/build-cxl -j --target llama-cli

../llama.cpp-cxl/build-cxl/bin/llama-cli \
    -m /path/to/model.gguf \
    --cxl-kv \
    --cxl-kv-device CXL0 \
    --cxl-kv-prefetch \
    -p "prompt"
```

`--cxl-kv` sets the internal `LLAMA_CXL_KV=1` path, allocates K/V cache tensors
with the ggml-cxl buffer type, initializes the selected CXL backend for KV-only
offload, and issues Type 2 cache-prefetch commands for K/V attention views unless
`--no-cxl-kv-prefetch` is used.

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

## Tests

Configure with CMake and run the self-contained unit tests through CTest:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The top-level CMake file builds every test source in `tests/`:

| Target | Purpose | CTest |
| --- | --- | --- |
| `test_dcd_gfam` | Standalone DCD allocation and GFAM access-control checks. | Yes |
| `test_rob` | Parses O3CPU/LSQ debug lines and feeds derived instructions into the ROB model. | Yes |
| `test_mem_stall` | Compares O3CPU memory-stall debug intervals against modeled ROB stalls. | Yes |
| `test_bandwidth_model` | Checks MLC-style bandwidth saturation and subtree-scoped fabric accounting. | Yes |
| `test_distributed_shm` | Two in-process distributed memory servers over SHM/TCP helper paths. | Built only |
| `test_back_invalidation` | TCP/PGAS client for external back-invalidation experiments. | Built only |
| `test_dax_back_invalidation` | DAX plus TCP client for external guest/device experiments. | Built only |

`test_rob` and `test_mem_stall` use gem5-style O3CPU, LSQ, and MemDepUnit debug text. Generate compatible traces with gem5 debug flags such as `O3CPU,LSQ,MemDepUnit`.

## gem5-CXL Integration

`gem5_integration/run.py` is a small launcher for SlugLab's gem5-CXL fork at <https://github.com/SlugLab/gem5-CXL>. It locates a gem5 binary, selects a config script, sets CXLMemSim environment variables, and enables O3CPU trace debug output by default.

Dry-run example:

```bash
./gem5_integration/run.py \
  --gem5-root /path/to/gem5-CXL \
  --config /path/to/gem5-CXL/configs/example/se.py \
  --cmd /bin/true \
  --enable-dcd \
  --enable-gfam \
  --dry-run
```

Full-system runs can pass kernel, disk image, workload script, memory size, CXL memory size, and extra gem5 config arguments:

```bash
./gem5_integration/run.py \
  --gem5-root /path/to/gem5-CXL \
  --kernel /path/to/vmlinux \
  --disk-image /path/to/disk.img \
  --script boot.rcS \
  --mem-size 4GB \
  --cxl-mem-size 16GB \
  --cxl-host 127.0.0.1 \
  --cxl-port 9999 \
  --enable-dcd \
  --enable-gfam \
  -- --cpu-type=O3CPU
```

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
  --mlc-bandwidth 92,78,64,0.82 \
  --bandwidth-window-ns 100000 \
  -w 85.5,86.5,87.5,85.5,86.5,87.5,88 \
  -o "(1,(2,3))"
```

### MLC-Calibrated Bandwidth Model

The legacy simulator can use Intel MLC-style measured bandwidth points instead of a fixed linear bandwidth penalty. Pass read-only, write-only, and mixed read/write peak bandwidth in GB/s:

```bash
--mlc-bandwidth <read_gbps>,<write_gbps>,<mixed_gbps>[,<knee_ratio>]
```

The model applies independently at each CXL memory expander and at each switch in the CXL fabric. That means bandwidth saturation can be charged at a device, a leaf switch, an upstream switch, or the root fabric path, depending on which subtree owns the cache-line addresses. `--bandwidth-knee` controls where latency starts rising sharply, and `--bandwidth-window-ns` sets the minimum accounting window used to avoid unstable penalties from very short bursts.

Common options:

| Option | Meaning |
| --- | --- |
| `-t` | Target executable. |
| `-i` | Simulator epoch or interval in milliseconds. |
| `-c` | CPU set used to run the target and pin remaining work. |
| `-d` | Platform DRAM latency in ns. |
| `-b` | Read/write bandwidth vector. |
| `-l` | Read/write latency vector. |
| `--mlc-bandwidth` | Read, write, mixed MLC peak bandwidth in GB/s, plus optional knee ratio. |
| `--bandwidth-knee` | Utilization where the MLC-style latency curve starts bending upward. |
| `--bandwidth-window-ns` | Minimum accounting window for bandwidth saturation calculations. |
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
