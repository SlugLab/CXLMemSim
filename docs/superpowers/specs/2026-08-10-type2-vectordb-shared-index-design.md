# Type-2 VectorDB Shared-Index Experiment Design

## Objective and proof boundary

The experiment evaluates a VectorDB exact-search workload whose float32 embedding matrix is updated by the guest CPU
and searched by a real GPU kernel through the QEMU Type-2 hetGPU backend. The matrix is the shared "weights" object.
The primary comparison is Splash Type-2 hardware-coherence emulation versus a software-managed coherence baseline.
A full-copy baseline and a native GPU control are retained to make synchronization costs interpretable.

An experiment may be labeled `type2-hwcc` only when all of the following are true:

1. The guest CPU and the real GPU kernel access one physical QEMU Type-2 backing allocation, not two replicas.
2. Guest CPU accesses enter the host endpoint and GPU range acquisitions enter the device endpoint of the same
   protocol-v2 MESI directory.
3. CPU updates after a GPU read produce nonzero ownership-transfer evidence: GETM or UPGRADE plus a device snoop ACK.
4. The GPU observes every committed CPU update and exact top-k output matches the CPU oracle.
5. Disabling the required coherence or synchronization operation causes the negative control to observe stale output.

The real GPU executes the distance and top-k kernels. Splash functionally models Type-2 line ownership and snoops; it
does not claim that the host RTX PRO 6000 physically speaks CXL.cache or that QEMU can observe individual GPU cache
misses after a line has been granted to the modeled device cache.

## Shared allocation architecture

The existing coherent pool cannot be used as evidence because its high-priority RAM subregion bypasses QEMU's
coherency callbacks. The replacement keeps RAM as an off-tree backing store but exposes the guest mapping through an
I/O MemoryRegion overlay. CPU reads and writes therefore use the Type-2 host endpoint before touching the backing
bytes. The QEMU process registers the same backing pages with the CUDA driver and obtains a device-visible address for
hetGPU kernels. No second data allocation is permitted in `type2-hwcc` mode.

The modeled GPU endpoint explicitly acquires the cache lines in a kernel's declared read or write range before launch.
For exact L2 search the database range is read-only during a kernel, so the first launch issues GETS and later launches
reuse valid device-cache entries. CPU updates serialize between kernels. A CPU update to a device-cached line follows
GETM/UPGRADE, synchronous snoop invalidation, ACK, and commit before the guest store completes. The next GPU launch
reacquires invalid lines before reading them. Kernel completion and endpoint release use system-scope CUDA and Type-2
fences.

Range acquisition is a frontend optimization only: it may batch transport frames, but the server expands the request
to aligned 64-byte lines, locks and transitions each directory entry independently, and reports per-line counters.
Partial, malformed, or timed-out range grants fail closed and prevent kernel launch.

## Compared modes

All modes execute the same CUDA exact-L2 and deterministic top-k implementation.

- `type2-hwcc`: one Type-2 allocation shared by CPU and GPU. The application performs no data copy and no explicit
  cache flush or invalidate. Endpoint ownership and snoops provide visibility.
- `software-cc`: a CPU Type-2 source matrix and a separate GPU replica. CPU updates set a cache-line dirty bitmap,
  flush each dirty CPU line with CLFLUSHOPT plus SFENCE, coalesce adjacent dirty rows, copy only those ranges H2D, and
  synchronize the CUDA stream before search. GPU result metadata is copied back and invalidated before CPU use.
- `full-copy`: the same two allocations, but the complete matrix is copied H2D after every update epoch. This bounds
  the cost of a conventional conservative synchronization policy.
- `native-gpu`: a device-resident matrix with no Type-2 path. It is a compute ceiling, not a coherence baseline.
- `negative-stale`: two replicas with CPU updates but no dirty-range propagation. At least one deliberately targeted
  query must return an old nearest neighbor; otherwise the correctness oracle is not sensitive enough.

Software-CC time includes dirty tracking, flush/fence, H2D transfer, and synchronization. Hardware-CC time includes
the synchronous directory and snoop work on the CPU writer's critical path and GPU reacquisition before launch.

## Workload

The database is a row-major float32 matrix with dimension 128 and deterministic seeded contents. Each epoch has three
ordered phases:

1. The CPU updates a deterministic set of rows and moves selected rows across a query's nearest-neighbor boundary.
2. The selected coherence mode publishes those updates.
3. The GPU executes exact L2 search and returns top-10 labels and distances for the query batch.

The default paper sweep uses:

- index rows: 16K, 64K, and 256K (8 MiB, 32 MiB, and 128 MiB);
- CPU update ratios per epoch: 0, 0.01, 0.1, and 1 percent;
- query batches: 1, 16, and 64;
- five warm-up epochs followed by ten measured epochs per point;
- fixed seeds recorded in every result row.

The smoke gate uses 4K rows, eight queries, and one updated row. Larger points run only after the smoke gate proves
correctness and nonzero protocol evidence. If runtime makes the full Cartesian product impractical, the required paper
subset is index-size scaling at 0.1 percent updates, update-ratio scaling at 64K rows, and batch scaling at 64K rows.

## Metrics and artifacts

Every measured epoch emits one JSONL record containing configuration, GPU identity, QEMU and server commit IDs,
end-to-end epoch latency, update time, synchronization time, kernel time, QPS, p50 and p99 query latency, bytes copied,
dirty lines, GETS, GETM, UPGRADE, PUTS, PUTM, snoop type and ACK counts, directory occupancy and evictions, and top-k
correctness. A run summary CSV contains medians and p25-p75 intervals. Raw stdout, server logs, QEMU logs, guest logs,
machine inventory, and exact commands are archived under a timestamped `artifact/type2_vectordb/` directory.

The primary paper figure plots end-to-end QPS for `type2-hwcc`, `software-cc`, and `full-copy` against update ratio.
A second panel reports synchronization bytes and time. Native GPU is shown as an upper-bound marker. Claims use ratios
and confidence intervals; absolute modeled Type-2 timing is identified as QEMU functional-emulation timing.

## Correctness and failure gates

The experiment fails rather than publishing a row when any gate is violated:

- CPU exact search and GPU top-10 labels differ, or distances exceed the configured floating-point tolerance.
- An updated row is not visible to the next GPU epoch.
- `type2-hwcc` reports zero directory transitions, zero post-update snoop ACKs, or nonzero copied data bytes.
- `software-cc` copies bytes outside the dirty-row envelope or reports zero copied bytes for a nonzero update epoch.
- The negative control does not produce a stale nearest neighbor.
- A line grant is partial, times out, or commits without all required snoop ACKs.
- A run silently falls back to CPU search, a mock GPU backend, protocol v1, or a separate GPU shadow allocation.

Unit tests cover allocation mapping, line alignment, dirty-range coalescing, range-acquire partial failure, and result
schema validation. QEMU qtests prove that guest CPU pool accesses no longer take the RAM bypass. The end-to-end gate is
a guest run using the real hetGPU backend and the real CXLMemSim server, followed by a counter and correctness audit.

## Repository ownership

CXLMemSim owns protocol-v2 range acquisition, result auditing, the experiment runner, plotting, and artifact schema.
The QEMU submodule owns the Type-2 backing/overlay split, host and device endpoint routing, CUDA host registration, and
kernel-launch integration. The Overleaf paper repository receives only figures, a concise methodology description,
measured results, and explicit limitations after the experiment gates pass. Server, QEMU, and paper changes are kept
on separate branches and pushed with the recorded commit IDs.
