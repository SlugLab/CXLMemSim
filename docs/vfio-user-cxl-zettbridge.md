# vfio-user CXL path for Zettbridge

## Current boundary

The working Zettbridge pair starts two QEMU processes and boots a Linux guest
in each. The consumer uses a standard QEMU CXL Type 3 endpoint, CFMWS and HDM
decoder. `cxl_type3_read/write()` translates the guest HPA to DPA and calls
`zettbridge_access()`. The provider QEMU owns the PCI requester, Intel vIOMMU
and ATS invalidation notifier. A private, fixed 96-byte Unix SEQPACKET protocol
connects the two QEMU processes. The Rust modules own the lease lifecycle in
their respective guest kernels.

QEMU's `vfio-user-pci` client currently carries PCI config space, BAR access,
interrupts, DMA map/unmap and server initiated DMA read/write. It does not
register an endpoint with QEMU's CXL fixed memory window routing. In this tree,
`cxl_cfmws_find_device()` accepts only `cxl-type3`, so exposing CXL DVSECs and
BARs through an ordinary `vfio-user-pci` device is insufficient: guest loads
to the CXL.mem aperture cannot reach the vfio-user server. The current
vfio-user protocol also has no CXL.mem transaction or ATS invalidation message.

## Working transport

`--transport vfio-user-cxl` now launches a separate
`~/zettbridge/tools/vfio_user_cxl_backend.py` process between the consumer
and provider QEMU processes. The consumer's `zettbridge` PCI function remains
inside QEMU, so the existing Type 3 frontend still owns CXL discovery and HDM
translation. The vfio-user stream carries management BAR0 through standard
`VFIO_USER_REGION_READ/WRITE` and CXL.mem through private commands `0x8001`
(read) and `0x8002` (write). Both private commands use the standard region
read/write body: native-endian `offset: u64`, `region: u32` (zero), `count: u32`,
followed by `count` data bytes on write and in the read reply. `offset` is DPA;
`count` is 1..64 and must fit in the configured aperture. An error reply has
only the 16-byte vfio-user header and a positive errno. This model requires a
little-endian host, matching its x86-64 guest test profile.

The connection must begin with `VFIO_USER_VERSION` 0.0 and the exact JSON
capability `{"capabilities":{"zettbridge_cxl":1}}`. The backend echoes this
capability before accepting any region or CXL.mem operations. It also answers
`VFIO_USER_DEVICE_GET_INFO` and `VFIO_USER_DEVICE_GET_REGION_INFO` for its
management function. No guest RAM file descriptors or host pointers cross the
transport. The backend relays each bounded operation to the provider QEMU's
existing local SEQPACKET endpoint; that QEMU still performs IOMMU checked DMA,
ATS translation and invalidation. Loss of either socket closes the session and
retains the provider's existing fault/quarantine behavior.

This is a Zettbridge-private vfio-user extension. It is not an upstream
vfio-user CXL standard and is not a generic `vfio-user-pci` Type 3 endpoint.

Run the model with:

```sh
ninja -C lib/qemu/build qemu-system-x86_64
./qemu_integration/run_zettbridge_pair.sh --transport vfio-user-cxl \
    --output /tmp/zettbridge-vfio-user-demo
```

## Path to a general vfio-user CXL device

1. Add a `vfio-user-cxl-type3` QEMU device that participates in the existing
   CXL host bridge, root port, fixed window and HDM routing. Reuse the existing
   Type 3 decoder validation and HPA-to-DPA logic. Keep CXL.io config/BAR/IRQ
   operations on the standard vfio-user PCI path.
2. Standardize a versioned `cxl-mem` capability during `VFIO_USER_VERSION`, with
   a bounded maximum transaction size. Define read/write requests carrying
   DPA, width, byte enables and payload, and replies carrying status plus read
   data. Reject unsupported capabilities, overflow, out-of-aperture accesses,
   malformed lengths and stale decoder epochs. Reads and writes must preserve
   QEMU's `MemTxResult` semantics. The private commands above are a functional
   starting point, not upstream-assigned command numbers.
3. Build a Zettbridge vfio-user backend that owns the Type 3 media/control
   implementation. The backend must keep the frozen v0.2 BAR/UAPI and the
   RESERVE, PREPARE, COMMIT, ATTACH, QUIESCE, DRAIN, RETIRE and RECLAIM order.
   It must never expose provider host pointers or use a shared payload file.
4. Preserve the provider requester domain. Standard vfio-user DMA read/write
   can perform vIOMMU checked DMA, but does not by itself provide this model's
   ATS translation cache, invalidation notifier or translated DMA behavior.
   Keep the existing provider QEMU requester until a separately negotiated ATS
   request/invalidate contract is implemented and verified. Do not claim ATS
   parity for a backend that only uses ordinary vfio-user DMA.
5. Launch two guest kernels with role-specific kernel images, initramfs files
   and `zettbridge.ko` modules. The provider and consumer may use different
   kernel trees; their module vermagic must match each selected kernel.

## Acceptance gates

- Start the vfio-user backend before the consumer QEMU; verify CXL discovery,
  CFMWS/HDM commit and a managed DAX mapping through the kernel driver.
- Read/write across the whole aperture, including unaligned and boundary
  accesses; reject out-of-range and unpublished DPA requests.
- Run the existing Zettbridge completion-loss, query, lease release, peer
  disconnect, reset and ATS invalidation tests against the new transport.
- Confirm each guest uses its selected kernel and module hashes from `run.json`.
  Test mismatched vermagic rejection before starting either QEMU.
- Measure the per-access vfio-user cost separately from the current in-QEMU
  model. Neither path is a hardware CXL latency measurement.

## Validation on 2026-11-04

- QEMU `qemu-system-x86_64` built. The Zettbridge management, ATS and edge
  qtests passed, including invalidations, reset, disconnect and malformed
  messages.
- The vfio-user pair printed `PAIR PASS`, with `CONSUMER CXL_DATA_PASS`,
  `PROVIDER DMA_FULL_RANGE_PASS` and `ATS_CONTINUOUS_PASS` in guest logs.
- Completion loss with an unknown query printed `QUARANTINE PASS`; the provider
  retained its quarantine path. The native pair also printed `PAIR PASS`.
- A vfio-user pair with separate provider and consumer module paths generated
  two initramfs files and passed. Both modules were built for the same available
  kernel; different kernel builds have not been exercised here.
- The backend framing/bounds unit tests passed. Each completed pair's QEMU and
  backend processes exited; persistent-mode stop was also verified.

The dual-kernel launcher and private vfio-user CXL transport are implemented.
The general device type, upstream protocol definition, fully external Type 3
device model and ATS transport parity described above remain implementation work.

References: [QEMU vfio-user protocol](https://www.qemu.org/docs/master/interop/vfio-user.html),
[QEMU CXL model](https://www.qemu.org/docs/master/system/devices/cxl.html),
[libvfio-user](https://qemu.googlesource.com/libvfio-user/+/refs/heads/master/README.md).
