# QEMU CXL Memory Simulator Integration

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
# Set environment variables
export CXL_MEMSIM_HOST=192.168.1.100  # IP of CXLMemSim server
export CXL_MEMSIM_PORT=9999

# Launch QEMU
./launch_qemu_cxl.sh
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