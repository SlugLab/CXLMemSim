# Real CXL.mem Host-Cache Coherence Validation Design

## Objective

Validate, on the locally attached physical CXL memory device, the host-facing
coherence mechanisms that a Type-2 device would also exercise through
`CXL.mem`. The validation must distinguish cache coherence evidence from the
existing LogP transport-delay model and must not claim validation of the
device-facing `CXL.cache` protocol.

This work also makes server shutdown statistics reliable and machine-readable
so that experiment artifacts contain the counters observed at process exit.

## Claim Boundary

The installed Montage device at `0000:64:00.0` advertises `Cache- IO+ Mem+`.
It is therefore a CXL 2.0 Type-3 memory device, not a Type-2 accelerator. The
experiment may establish all of the following:

- CPU cache lines backed by physical CXL.mem remain coherent across cores and
  NUMA domains without software cache-line flushes.
- Cold cache misses are supplied by the CXL.mem device.
- A modified line can move directly between CPU caches through the host
  coherence fabric while its home address remains in CXL.mem.
- C11 atomic read-modify-write operations on the mapped CXL.mem range are
  linearizable among host CPUs.

The experiment cannot establish any of the following:

- device-initiated `CXL.cache` D2H requests;
- H2D snoops to a Type-2 device cache;
- DCOH directory capacity, replacement, or ownership metadata;
- host-bias/device-bias transitions; or
- coherent access by the installed PCIe NVIDIA GPU.

The paper must call this a real-hardware decomposition validation of the
host-facing CXL.mem slice, not full Type-2 hardware validation. Existing
VectorDB results must be labeled `emulated Type-2 HWCC`.

## Hardware Safety Contract

The runner opens `/dev/dax0.0` only after all checks pass:

1. `mem0` resolves to PCI function `0000:64:00.0`.
2. PCI identity is `1b00:c002`, the CXL DVSEC reports `Cache-` and `Mem+`, and
   the serial is `0x8a0af738c2820407`.
3. `region0` is committed RAM in devdax mode, has a base resource of
   `0x2080000000`, and is at least 128 GiB.
4. `/dev/dax0.0` has no open file descriptors according to both `fuser` and
   `/proc/*/fd` inspection, excluding the runner itself after acquisition.
5. The requested offset is exactly zero and the mapping length is no more than
   2 MiB.
6. The runner obtains an exclusive advisory lock before mapping and records all
   attested values in the artifact manifest.

Any mismatch fails closed before `mmap` or the first store. The benchmark only
writes the approved first 2 MiB and does not alter region configuration,
decoder state, namespaces, firmware, or PCI configuration space.

## Experiment Architecture

### Benchmark executable

Add `microbench/cxlmem_host_cc_validation.c`, built with C11 atomics and
`pthread`. It supports one subcommand per measurement so Linux `perf stat` can
attribute counters to a single operation class. Every result is printed as one
JSON object.

The benchmark maps either the attested DAX range or an anonymous DRAM control,
pins workers to explicit CPUs, prefaults pages, and uses `RDTSCP` plus
`CLOCK_MONOTONIC_RAW`. It reports both cycles and nanoseconds. Correctness
state occupies isolated 64-byte cache lines.

### Experiment runner

Add `script/run_cxlmem_hwcc_validation.py`. It performs hardware attestation,
builds the benchmark, chooses distinct physical cores, executes repetitions,
collects `perf` output, validates all invariants, and writes an immutable run
directory under `artifact/cxlmem_hwcc/<run-id>/`.

Each run contains:

- `manifest.json` with git commit, kernel, CPU, BDF, DVSEC, serial, firmware,
  region, DAX mapping, CPU placement, command lines, and safety checks;
- raw benchmark JSONL and normalized `results.csv`;
- raw `perf stat` files and normalized PMU counters;
- captured `lspci`, `cxl list`, `daxctl list`, `numactl`, and sysfs state;
- `summary.json`, `validation.json`, and SHA-256 checksums.

The validator can run independently with `--validate-only RUN_DIR` and does
not access the DAX device.

## Measurements and Proof Gates

### 1. Address-source attestation

Run warm and explicitly cold dependent loads against DRAM and CXL.mem. The
cold CXL.mem case evicts benchmark lines before timing; the warm case performs
no flush inside the measured loop.

Required evidence:

- `mem_load_retired.local_cxl_mem` or the equivalent supported offcore event is
  nonzero for cold CXL.mem loads;
- CXL PMU `m2s_req_memrd*` and/or `s2m_drs_memdata` increases during the cold
  CXL.mem phase; and
- warm CXL.mem accesses have materially fewer CXL read requests per operation
  than cold CXL.mem accesses.

This gate establishes that the mapping reaches physical CXL.mem and that warm
accesses can be served above the media path.

### 2. No-flush message-passing litmus

Producer and consumer threads communicate through data and flag lines in the
CXL.mem mapping. The producer writes a sequence and publishes it with a C11
release store; the consumer observes it with an acquire load and verifies the
corresponding payload. The installed one-socket SNC topology is tested on two
cores in one NUMA node and across its two NUMA nodes. No `CLFLUSH`,
`CLFLUSHOPT`, or `CLWB` appears in this path.

Required evidence:

- zero stale payloads, regressions, or timeouts across all measured iterations;
- the exact expected final sequence; and
- the same test contract passes for the DRAM control.

### 3. Ownership handoff

Two pinned threads alternate ownership of one CXL.mem-backed cache line using
release/acquire atomics. Each turn writes and checks a monotonically increasing
token. Same-NUMA and cross-NUMA variants report handoff latency and
throughput.

Required evidence:

- zero token errors and exact completion count;
- nonzero `mem_load_l3_hit_retired.xsnp_fwd`, an equivalent local HITM event,
  or a supported CHA snoop-forward event for the same-NUMA and cross-NUMA core
  pairs; and
- CXL media reads per handoff remain below the explicitly cold CXL.mem case.

Unavailable model-specific PMU events are recorded as unsupported, but the
run is publication-ready only if at least one same-NUMA or cross-NUMA
cache-to-cache ownership event is observed.

### 4. Atomic linearizability

Two threads execute C11 sequentially consistent fetch-add and compare/exchange
operations on isolated CXL.mem cache lines. The benchmark checks the final
value, the set of returned old values, and CAS success/failure accounting.

Required evidence:

- bit-exact final values and no duplicate or missing fetch-add tickets;
- CAS accounting sums to the attempted operations; and
- no cache flush instruction is used in the atomic hot path.

## Statistics-at-Exit Contract

The current server prints final statistics inside `stop()` only when
`running.exchange(false)` returns true. Cleanup and reporting will be split so
that cleanup remains idempotent and a separate `printFinalStatsOnce()` emits a
single final snapshot on normal return, SIGINT, or SIGTERM, including PGAS_SHM.

The snapshot includes server operations, coherence-v2 transition/audit
counters, and controller topology counters. A `--stats-json PATH` option writes
the same snapshot atomically as JSON. Human-readable and JSON values must
agree.

Before production changes, an integration test starts a PGAS_SHM server,
executes READ, WRITE, FAA, CAS, and FENCE requests, sends SIGTERM, and asserts:

- the process exits cleanly;
- final statistics appear exactly once;
- all exercised counters are nonzero and internally consistent; and
- controller remote, endpoint load/store, and thread counts reflect traffic.

The same test covers a second `stop()` call to ensure it cannot suppress or
duplicate reporting.

## Paper Integration

After the artifact passes independent validation, update
`pact_draft_jesun.tex` with:

1. an experimental-setup paragraph attesting the physical Type-3 hardware;
2. a real-hardware CXL.mem host-coherence validation subsection;
3. a table or compact figure reporting warm/cold latency, ownership handoff,
   litmus correctness, atomic correctness, and PMU evidence;
4. a statement that LogP models transport delay and is not coherence evidence;
5. replacement of ambiguous `Type-2 HWCC` labels with `emulated Type-2 HWCC`;
6. a limitation stating that `CXL.cache`, DCOH, bias, and a coherent GPU agent
   remain unvalidated on physical Type-2 hardware.

No numerical result is written before the run's `validation.json` reports
`pass`. The paper records the exact source commit and run-directory checksum.

## Verification

Completion requires all of the following:

- the PGAS exit-statistics negative test fails before the fix and passes after;
- Release and Debug builds pass;
- the existing CTest suite remains green;
- the hardware run stays inside the 2 MiB boundary and passes all mandatory
  correctness and PMU gates;
- independent artifact validation passes;
- the paper builds without new warnings or undefined references; and
- source, QEMU gitlink if changed, paper, and audit artifacts are pushed to
  their respective remote branches.
