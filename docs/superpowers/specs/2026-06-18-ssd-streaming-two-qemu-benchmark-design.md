# End-to-End SSD Streaming Benchmark for Two QEMU CXL PMEM Guests

## Objective

Benchmark the end-to-end SSD streaming path where two QEMU guests expose CXL
persistent memory and access it through guest DAX devices. The server storage
backend must use `/mnt/disk0/cxlmemsim-ssd-bench.swap` via
`cxlmemsim_server --ssd-backing-file`.

## Architecture

The benchmark has one host-side CXLMemSim server and two local QEMU guests. The
server owns the persistent SSD streaming backing file and serves CXL Type 3
memory traffic. Each guest boots with a CXL persistent memory device, configures
the CXL region inside Linux, and exposes `/dev/dax0.0`.

The first implementation should reuse the valid Type 3 launcher
`qemu_integration/launch_qemu_cxl_usernet.sh` with per-guest environment
overrides. Existing stale helpers that reference missing launchers or another
checkout path are not part of the benchmark path.

## Components

- Host launcher: starts `build/cxlmemsim_server` with SSD streaming enabled,
  starts two QEMU instances, records PIDs, and writes logs under
  `artifact/ssd_stream_bench/`.
- Guest setup: runs `qemu_integration/setup_cxl_numa.sh` in each guest with
  per-node settings so `/dev/dax0.0` is available.
- Guest stream worker: performs sequential write/read/verify operations against
  `/dev/dax0.0` with bounded sizes.
- Result collector: records per-direction elapsed time, throughput, verification
  status, server configuration, QEMU logs, and guest setup logs.

## Data Flow

1. Host starts `cxlmemsim_server` with `--comm-mode=tcp`, a selected capacity,
   and `--ssd-backing-file=/mnt/disk0/cxlmemsim-ssd-bench.swap`.
2. Host starts two QEMU guests with separate `SSH_PORT`, `CXL_HOST_ID`, CXL
   backing paths, and LSA paths.
3. Guest setup creates the CXL pmem/DAX region in both VMs.
4. Node 0 writes a sequential pattern to `/dev/dax0.0`; Node 1 reads the same
   offset and verifies the pattern.
5. Node 1 writes a different sequential pattern; Node 0 reads and verifies it.
6. The collector computes MiB/s for each write/read direction and stores raw
   logs for later inspection.

## Defaults

- Backing file: `/mnt/disk0/cxlmemsim-ssd-bench.swap`.
- Artifact directory: `artifact/ssd_stream_bench/YYYYmmdd-HHMMSS/`.
- Initial transfer size: 256 MiB unless overridden.
- CXL/server capacity: at least the transfer size, rounded up to a practical
  MiB value.
- QEMU network: user-mode SSH forwarding with distinct host ports for each VM.

## Error Handling

The harness should fail fast when required binaries or images are missing,
server startup fails, QEMU exits early, SSH never becomes reachable, guest CXL
setup fails, `/dev/dax0.0` is absent, or pattern verification fails. Logs should
be left in the artifact directory on failure.

## Testing And Validation

Validation has two levels:

- Static checks: shell syntax check for new scripts and build checks for any
  new microbenchmark binaries.
- Live check: one bounded two-direction stream run through both QEMU guests,
  with data verification and recorded throughput.

## Non-Goals

This design does not optimize the SSD backend, change QEMU device models, or
replace the existing CXL setup helper. It also does not run destructive raw block
device tests; the approved backing path is a regular file on `/mnt/disk0`.
