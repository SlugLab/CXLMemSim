# Zettai RPU v0.2: Two QEMU Guests

English | [简体中文](README_zettbridge.zh-CN.md)

The Rust out-of-tree driver lives in `~/zettbridge` and targets the kernel in
`~/linux`. Host A exposes a standard CXL Type 3 device. Each host also has a
separate `zettbridge` management function; the Host B function performs DMA
through its IOMMU.

The driver is in [Zettai-US/zettbridge](https://github.com/Zettai-US/zettbridge).
This integration pins driver commit `a1071003b899fbdc1a1ac35f9ea6b398daee5a09`
and QEMU submodule commit `c16508c908fc5761ea158d8386db738978c36390`.
For a fresh environment:

```sh
git clone git@github.com:Zettai-US/zettbridge.git ~/zettbridge
git -C ~/zettbridge checkout a1071003b899fbdc1a1ac35f9ea6b398daee5a09
git -C ~/CXLMemSim submodule update --init lib/qemu
```

Build and run the basic pair:

```sh
make -C ~/zettbridge KDIR=~/linux LLVM=1
make -C ~/zettbridge tools test
ninja -C lib/qemu/build qemu-system-x86_64 -j 12
./qemu_integration/run_zettbridge_pair.sh --output /tmp/zettbridge-demo
```

The launcher boots two VMs with separate RAM and temporary initramfs images.
It tests CXL mmap, usage relay and safe reclamation without the existing disk
image. Success prints `PAIR PASS`; the output directory retains both guests'
logs and exact launch commands.

## Failure and lifecycle tests

```sh
# Recover after a lost ACK by querying the result.
./qemu_integration/run_zettbridge_pair.sh --drop-commit-ack --output /tmp/zettbridge-ack
# Keep backing and DMA mappings when the result cannot be determined.
./qemu_integration/run_zettbridge_pair.sh --drop-commit-ack --query-unknown \
    --output /tmp/zettbridge-quarantine
# Check Rust FD, PCI unbind and module unload lifetime.
./qemu_integration/run_zettbridge_pair.sh --lifetime --output /tmp/zettbridge-lifetime
# Management ABI regression using two qtest QEMUs; no guest kernel needed.
python3 lib/qemu/tests/qtest/zettbridge-test.py lib/qemu/build/qemu-system-x86_64
# ATS translation, invalidation, permissions and reclamation.
python3 lib/qemu/tests/qtest/zettbridge-ats-test.py lib/qemu/build/qemu-system-x86_64
```

## Larger persistent pair

```sh
./qemu_integration/run_zettbridge_pair.sh --ats on --capacity 8G \
    --provider-memory 12G --consumer-memory 4G --keep-running \
    --output /tmp/zettbridge-8g
python3 ~/zettbridge/tools/guest_console.py /tmp/zettbridge-8g consumer \
    --command 'zbctl consumer-stats'
python3 ~/zettbridge/tools/stop_pair.py /tmp/zettbridge-8g
```

`PAIR RUNNING` means that windowed sampling over the full aperture and
continuous reads/writes are ready. Both QEMU processes remain running. Serial
consoles and QMP use local Unix sockets in the output directory; no network
port is opened. Validated pool sizes are 256 MiB and 8 GiB. A lease is limited
to 4 GiB, so larger pools use multiple leases.

`ZETTBRIDGE_DIR`, `KDIR` and `QEMU_BINARY` override the default paths. See the
driver repository's README and `docs/` for the UAPI, unsafe and lock-order
audit, deployment manifest, and the validated and unvalidated K0–K6 boundaries.

## Separate kernel builds

The two guests may use different built Linux kernel trees. Build a matching
`zettbridge.ko` for each kernel. The launcher checks module vermagic before
boot and packages a separate initramfs for each guest:

```sh
PROVIDER_KDIR=/path/to/provider-linux \
CONSUMER_KDIR=/path/to/consumer-linux \
PROVIDER_ZETTBRIDGE_KO=/path/to/provider/zettbridge.ko \
CONSUMER_ZETTBRIDGE_KO=/path/to/consumer/zettbridge.ko \
./qemu_integration/run_zettbridge_pair.sh --output /tmp/zettbridge-two-kernels
```

`run.json` records each role's kernel release, source commit, kernel image
SHA-256 and module SHA-256 under `kernels.provider` and `kernels.consumer`.
Without these overrides, both guests use the default kernel build and module.

## Zettbridge vfio-user CXL transport

```sh
ninja -C lib/qemu/build qemu-system-x86_64
./qemu_integration/run_zettbridge_pair.sh --transport vfio-user-cxl \
    --output /tmp/zettbridge-vfio-user-demo
```

This mode sends management BAR operations as standard vfio-user region
messages and CXL.mem DPA reads/writes as negotiated Zettbridge-private
commands. The separate backend relays them to Host B QEMU, which remains the
IOMMU/ATS requester. It is a Zettbridge-specific transport, not a generic
`vfio-user-pci` CXL Type 3 passthrough device. See the
[design and validation record](../docs/vfio-user-cxl-zettbridge.md).

## Model boundaries

The default profile is x86-64 with a 256 MiB or 8 GiB, single-way static
volatile CXL aperture, Host B DRAM backing, UC lease mappings, IOMMU and
continuously enabled ATS. `--ats off` reproduces the original baseline.
Host B caches translations in the requester domain and removes entries on
VT-d device invalidation. RETIRE flushes the ATC while ATS stays enabled for
DMA unmap/free. Disabling ATS during an active export faults it. The native
QEMU-to-QEMU link uses Unix SEQPACKET. Synchronous completion validates the
functional model; it does not establish physical FPGA posted-write drain or
hardware latency.

The driver uses the target kernel's native Rust PCI, MiscDevice, Arc/Mutex,
workqueue and DMA interfaces. Stateless C helpers cover missing kernel
macros/inlines and a private DAX ABI. The target kernel is not modified.

The Intel IOMMU driver in `~/linux` enables ATS; the Rust module verifies and
monitors it. PASID, PRI and SVA are disabled. Host A relays lease usage to
Host B about every 200 ms; reports become invalid after 2 seconds without an
update. Read-only `/sys/class/rpu/rpu0/ats_*` diagnostics report enabled,
requests, hits, invalidations, cached entries, faults and transitions. See the
[driver ATS documentation](https://github.com/Zettai-US/zettbridge/blob/main/docs/ats.md).

Boundary fixes cover Rust VMA revocation batches, a global revoke arriving
during a local revoke, idempotent fault handling, and QEMU ATS notifier
registration failure, reset before publication and disconnect before ATTACH.
Expired usage reports show `unknown` and become valid again after recovery.
Peer loss retains quarantined resources; ATS diagnostics no longer mistake
all-ones MMIO for enabled ATS. See the
[driver edge-case record](https://github.com/Zettai-US/zettbridge/blob/main/docs/edge-cases.md).

```sh
# Thirteen hardware-model edge tests.
python3 lib/qemu/tests/qtest/zettbridge-edge-test.py lib/qemu/build/qemu-system-x86_64
# New isolated pair: four threads, 128 mmap/reclaim rounds, usage expiry/recovery and peer-loss quarantine.
python3 ~/zettbridge/tools/edge_pair.py --output /tmp/zettbridge-edge-new
```

The edge script requires a fresh output directory and shuts down only the
instances it creates. Rebuilding does not replace the model or driver in a
running VM; a persistent 8 GiB pair continues using its original binaries.

## llama.cpp agent with directly shared KV

The driver repository includes an isolated test pinned to
llama.cpp/Qwen2.5-0.5B-Instruct Q8_0:

```sh
python3 ~/zettbridge/tools/build_llama_agent.py
python3 ~/zettbridge/tools/run_llama_agent.py \
    --model ~/zettbridge/artifacts/llama-models/qwen2.5-0.5b-instruct-q8_0.gguf \
    --output /tmp/zettbridge-agent-new
```

It starts two new VMs and performs the `read_order → multiply → final` sequence
on Host A. All 48 shared K/V tensors, totaling 3 MiB, stay in the Rust
consumer's lease mmap from context creation to destruction. Each CPU attention
worker uses only a 256-byte scratch row for its current K/V calculation. The
test compares every output token and full per-step logits fingerprint with a
local reference, samples Host B usage and ATS, then checks release and safe
reclamation.

The model and application are loaded from a read-only workload disk attached
only to the consumer; shared KV still travels through CXL, the QEMU bridge,
and Host B ATS/DMA. Success prints `LLAMA AGENT E2E PASS`. The output directory
retains `result.json`, both logs and binary checksums. UC MMIO timings are
functional results, not physical CXL performance measurements. See the
[driver llama.cpp record](https://github.com/Zettai-US/zettbridge/blob/main/docs/llama-agent.md).
