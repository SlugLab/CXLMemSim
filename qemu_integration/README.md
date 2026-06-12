#QEMU CXL Memory Simulator Integration

This directory contains the integration between QEMU and CXLMemSim, allowing QEMU to use remote CXL memory simulated by CXLMemSim over TCP.

## Architecture

1. **libCXLMemSim.so**: A shared library that QEMU loads to intercept CXL memory operations
2. **cxlmemsim_server**: A server that runs on the CXLMemSim host and handles memory requests
3. **TCP Protocol**: Simple request/response protocol for memory operations

## Building

### Using CMake (Recommended)

```bash
cd qemu_integration
mkdir build
cd build
cmake ..
make -j$(nproc)
sudo make install
```

### Using Make (Legacy)

```bash
cd qemu_integration
make
sudo make install
```

## Usage

### 1. Start the CXLMemSim Server

On the host running CXLMemSim:

```bash
./start_server.sh 9999 topology_simple.txt
```

### 2. Launch QEMU with CXL Memory

```bash
#Set environment variables
export CXL_MEMSIM_HOST=192.168.1.100  # IP of CXLMemSim server
export CXL_MEMSIM_PORT=9999

#Launch QEMU
./launch_qemu_cxl.sh
```

### VCS, DCD, and GFAM startup

Use `launch_qemu_vcs_dcd_gfam.sh` when testing the Zettai RFC VCS switch path with
CXLMemSim-owned DCD/GFAM behavior:

```bash
export DISK_IMAGE=/path/to/qemu.img
export CXL_MEMSIM_PORT=9999
export CXL_CAPACITY_MB=10240
./launch_qemu_vcs_dcd_gfam.sh
```

The script starts `build/cxlmemsim_server` if the TCP port is not already
listening, enables DCD and GFAM on the server, exports the matching QEMU
environment variables, and boots a two-USP `zettai` topology. The hidden
`cxl-type3` DCD devices are attached to the switch only after FMAPI `Bind vPPB`
commands. Keep `CXL_CAPACITY_MB`, `CXL_DC_SIZE`, and
`CXL_FMW_SIZE` aligned so QEMU's boot-time dynamic-capacity window matches the
CXLMemSim server-reported capacity.

The same launcher also creates a direct PCIe `cxl-type2` CXL.mem endpoint by
default. It is attached under a normal CXL root port, runs in simulation mode
(`gpu-mode=0`), and enables the Type2 DCD, GFAM, and MH-SLD models:

```bash
CXL_TYPE2_ENABLE=1 \
CXL_TYPE2_MEM_SIZE=4G \
CXL_TYPE2_DCD_INITIAL_SIZE=4G \
CXL_TYPE2_HOST_ID=0 \
CXL_TYPE2_HEAD_ID=0 \
./launch_qemu_vcs_dcd_gfam.sh
```

Set `CXL_TYPE2_ENABLE=0` to boot only the Zettai VCS Type3 DCD topology. The
Type2 control plane is exposed through the existing BAR2 command window. New
commands are available for dynamic capacity add/release, GFAM grant/revoke, and
MH-SLD status. Status registers report DCD total/allocated/free capacity, GFAM
host/mapping/denial counters, and MH-SLD head/conflict/invalidation counters.

Networking is disabled by default because this QEMU build may not include
SLIRP/user networking. Set `QEMU_NET_MODE=none` for CXL-only boot tests.
Set `QEMU_NET_MODE=passt` only when the host has the `passt` executable
installed. Use `QEMU_NET_MODE=tap` for a preconfigured tap device, or
`QEMU_NET_MODE=custom` with `QEMU_NETDEV='backend,id=net0,...'`.

The boot disk is created with `if=none` and an explicit
`virtio-blk-pci,drive=bootdisk,bus=pcie.0` device. This avoids QEMU placing the
implicit virtio disk on the CXL expander bus, where only PCI/PCIe bridges are
valid. Override the device with `QEMU_DISK_DEVICE=...` if your guest needs a
different boot controller. `DISK_FORMAT=auto` is the default and uses
`qemu-img info` when available. A raw filesystem image such as an ext4 rootfs is
not directly BIOS-bootable; use it with `KERNEL_IMAGE=/path/to/bzImage` or pass
a whole-disk image that contains a bootloader.

Inside the Linux guest, the kernel still exposes standard CXL names such as
`/sys/bus/cxl/devices/mem0`, `port0`, `endpoint0`, and `region0`. The QEMU
object name `zettai` does not rename those kernel classes. For guest scripts
that should use Zettai names without rebuilding Linux, run:

```bash
sudo ./zettai_linux_aliases.sh
ls -l /run/zettai/cxl
cat /run/zettai/topology.txt
```

This creates symlinks such as `/run/zettai/cxl/zettai-mem0` and records the
current CXL/DAX/ND topology. To refresh aliases on each guest boot, run
`sudo ./zettai_linux_aliases.sh --install-systemd` inside a systemd-based guest.
Changing the real kernel-visible class and driver names requires patching a
Linux kernel tree, for example the CXL core files under `drivers/cxl/`; this
repository does not include those kernel sources.

### Zettai DCD/GFAM smoke test

If `cxl list -B -D -P -M` only shows root decoders and switch ports, the guest
is still in the pre-bind state. Bind one hidden DCD endpoint from the host with
QMP, add a DCD extent, then create a Linux CXL region in the guest.
The Zettai switch mailbox CCI enumerates with PCI id `7a74:a123`; verify it in
the guest with `lspci -nn | grep -i '7a74:a123'`.

Launch QEMU with a QMP socket:

```bash
QEMU_NET_MODE=none \
KERNEL_IMAGE=/path/to/bzImage \
DISK_IMAGE=/path/to/rootfs.img \
CXL_DCD_INITIAL_CAPACITY_MB=0 \
QEMU_EXTRA_ARGS="-qmp unix:/tmp/zettai-qmp.sock,server=on,wait=off" \
./launch_qemu_vcs_dcd_gfam.sh
```

From the host:

```bash
python3 ./zettai_host_dcd_gfam_test.py --bind --add --query
```

This uses the QMP `zettai-bind-vppb` test hook to bind
`vcs-id=0,vppb-id=0` to `dsp-ppb-id=0`, then issues
`cxl-add-dynamic-capacity` for a 256 MiB extent. Use `--help` to change the
vPPB, host id, DCD QOM path, or extent size.

Inside the guest after the bind has hotplugged `mem0`:

```bash
sudo ./zettai_guest_dcd_gfam_test.sh
```

The Zettai guest helper is a thin wrapper around `setup_cxl_numa.sh`. The
common setup script selects a root decoder such as `decoder0.0`, creates a
single-way CXL `ram` region on `mem0`, creates a device-dax instance when
possible, and can write through `/dev/daxX.Y` so CXLMemSim GFAM read/write
counters move. You can run it directly when you do not need the DCD/GFAM smoke
touch:

```bash
sudo CXL_REGION_TYPE=ram CXL_DAX_MODE=devdax ./setup_cxl_numa.sh
sudo CXL_REGION_TYPE=ram CXL_DAX_MODE=system-ram ./setup_cxl_numa.sh
```

When installing `cxl-numa-setup.service` in a guest, put per-VM settings in
`/etc/default/cxl-numa-setup`. The setup helper derives a default static IP as
`192.168.100.(10 + CXL_HOST_ID)/24` when `CXL_CONFIGURE_NET=1` and
`CXL_NET_ADDR` is not set:

```bash
# Primary VM
CXL_CONFIGURE_NET=1
CXL_HOST_ID=0

# Secondary VM
CXL_CONFIGURE_NET=1
CXL_HOST_ID=1
```

Set `CXL_NET_ADDR` explicitly if the VM should use a different address. The
helper auto-detects the first non-loopback network interface by default; set
`CXL_NET_IFACE=enp0s2` or similar to pin it.

For DCD/volatile CXL.mem, use `daxctl`/device-dax or system-ram mode. The old
`ndctl create-namespace -m dax` path is for pmem-style regions and is disabled
by default in `setup_cxl_numa.sh`; enable it only with
`CXL_CREATE_NDCTL_NAMESPACE=1`.
Query counters again from the host with:

```bash
python3 ./zettai_host_dcd_gfam_test.py --query
```

## Features

- **Cacheline-granular access**: All memory operations are performed at 64-byte cacheline granularity
- **Hotness tracking**: Tracks access frequency for each 4KB page
- **Latency simulation**: Configurable latency models based on topology
- **Statistics**: Comprehensive access statistics and performance metrics
- **Multiple server implementations**:
  - `cxlmemsim_server`: Simple server with basic latency modeling
  - `cxlmemsim_server_advanced`: Advanced server with topology-aware latency

### Advanced Server Features

The advanced server (`cxlmemsim_server_advanced`) provides:
- Topology file support for configuring multiple memory tiers
- Per-page access statistics (read/write counts and bytes)
- Periodic statistics reporting
- Memory usage tracking
- Hot page identification

## Protocol

The TCP protocol uses fixed-size messages:

### Request (from QEMU to CXLMemSim)
- `op_type`: 0 for read, 1 for write
- `addr`: 64-bit physical address
- `size`: Size of operation (max 64 bytes)
- `timestamp`: Nanosecond timestamp
- `data`: Data for write operations

### Response (from CXLMemSim to QEMU)
- `status`: 0 for success
- `latency_ns`: Simulated latency in nanoseconds
- `data`: Data for read operations

## Memory Hotness Analysis

The system tracks memory hotness at page granularity (4KB). Access statistics include:
- Total reads and writes
- Per-page access counts
- Top hottest pages

Statistics are printed when QEMU exits and periodically by the server.

## Integration with QEMU CXL

The library intercepts `cxl_type3_read` and `cxl_type3_write` functions in QEMU's CXL emulation layer. When QEMU is launched with the LD_PRELOAD environment variable, these functions are redirected to communicate with the remote CXLMemSim server.

## Troubleshooting

1. **Connection refused**: Ensure the CXLMemSim server is running and accessible
2. **Performance**: For better performance, run the server on the same machine or low-latency network
3. **Memory size**: Ensure QEMU's CXL memory size matches the server's configuration
