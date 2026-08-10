# Type-2 VectorDB Shared-Index Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run a real-GPU exact VectorDB search whose embedding matrix is shared with the guest CPU through one QEMU Type-2 allocation, compare hardware-coherence emulation with software-managed coherence, and publish audited paper results.

**Architecture:** QEMU stores the coherent pool in off-tree RAM and exposes it to the guest through trapping I/O callbacks, while the CUDA backend registers those same host pages for GPU access. CXLMemSim protocol v2 remains the line-granular ownership authority; GPU kernels explicitly acquire declared ranges before launch, and software-CC uses a separate GPU replica plus dirty-line flush/copy synchronization.

**Tech Stack:** C++20/CMake/CTest, QEMU C/GLib/qtest, CUDA Driver API/PTX, Bash, Python 3, JSONL/CSV, matplotlib, LaTeX.

---

## File map

- `include/coherence_endpoint_cache.h`, `src/coherence_endpoint_cache.cpp`: fail-closed line-by-line range acquisition.
- `tests/test_coherence_range_acquire.cpp`: successful, unaligned, and partial-failure range semantics.
- `lib/qemu/include/hw/cxl/cxl_hetgpu.h`, `lib/qemu/hw/cxl/cxl_hetgpu.c`: CUDA host registration for coherent backing pages.
- `lib/qemu/include/hw/cxl/cxl_type2.h`, `lib/qemu/hw/cxl/cxl_type2.c`: off-tree backing, trapping guest overlay, range command dispatch, and lifecycle.
- `lib/qemu/include/hw/cxl/cxl_type2_gpu_cmd.h`: guest/QEMU range-acquire ABI.
- `lib/qemu/tests/qtest/cxl-type2-coherent-pool-test.c`, `lib/qemu/tests/qtest/meson.build`: prove pool accesses trap and invalid ranges fail.
- `qemu_integration/guest_libcuda/cxl_gpu_cmd.h`, `qemu_integration/guest_libcuda/libcuda.c`: guest range-acquire and mapped-pointer APIs.
- `qemu_integration/guest_libcuda/vectordb_shared_index.c`: deterministic exact-L2 workload and five experiment modes.
- `qemu_integration/guest_libcuda/Makefile`: build guest benchmark and static CPU oracle helper.
- `tests/test_vectordb_dirty_ranges.cpp`: software-CC dirty-line coalescing contract.
- `script/run_type2_vectordb.py`: build, launch, sweep, archive, and validate runs.
- `tests/test_run_type2_vectordb.py`: runner/schema negative tests.
- `script/plot_type2_vectordb.py`: paper figures from validated CSV only.
- `697a667df59bdd0085f8ce88/pact_draft_jesun.tex`: methodology and measured findings after all gates pass.

### Task 1: Establish isolated QEMU baseline

**Files:**
- Modify gitlink: `lib/qemu`

- [ ] **Step 1: Initialize the submodule without using the dirty primary checkout**

```bash
git submodule update --init lib/qemu
git -C lib/qemu fetch origin
git -C lib/qemu switch -c codex/type2-vectordb-shared-index-20260810 7a6d9f9c57
```

Expected: the QEMU branch starts at the pushed Type-2 integration commit and `git -C lib/qemu status --short` is empty.

- [ ] **Step 2: Configure and run the existing Type-2 qtest baseline**

```bash
mkdir -p build/qemu-vectordb
cd build/qemu-vectordb
../../lib/qemu/configure --target-list=x86_64-softmmu --enable-cxl --disable-werror
ninja qemu-system-x86_64
ninja test-qtest-x86_64
```

Expected: QEMU builds and existing CXL qtests pass before production edits.

- [ ] **Step 3: Record the QEMU baseline**

```bash
git -C lib/qemu rev-parse HEAD > build/qemu-vectordb/baseline.commit
git status --short --branch
```

Expected: only the initialized `lib/qemu` gitlink may appear changed in the superproject.

### Task 2: Add fail-closed range acquisition to endpoint caches

**Files:**
- Modify: `include/coherence_endpoint_cache.h`
- Modify: `src/coherence_endpoint_cache.cpp`
- Create: `tests/test_coherence_range_acquire.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing range tests**

```cpp
TEST_CASE("read range grants every aligned line") {
    auto result = endpoint.acquireRange(0x1000, 128, RangeIntent::Read);
    CHECK(result.status == protocol_v2::Status::Ok);
    CHECK(result.lines_granted == 2);
    CHECK(endpoint.contains(0x1000));
    CHECK(endpoint.contains(0x1040));
}

TEST_CASE("partial grant fails closed") {
    transport.failRequestForLine(0x1040);
    auto result = endpoint.acquireRange(0x1000, 128, RangeIntent::Read);
    CHECK(result.status == protocol_v2::Status::IoError);
    CHECK(result.lines_granted == 1);
    CHECK(result.complete == false);
}
```

Use the repository's existing assertion helpers rather than introducing Catch2 if it is not already linked. Also test zero size, overflow, and a range whose endpoints are not 64-byte aligned.

- [ ] **Step 2: Run the test to prove the API is absent**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCXLMEMSIM_ENABLE_SLUGALLOCATOR=OFF
cmake --build build --target test_coherence_range_acquire -j8
```

Expected: compile failure naming `RangeIntent` or `acquireRange`.

- [ ] **Step 3: Add the minimal public contract**

```cpp
enum class RangeIntent : std::uint8_t { Read, Write };

struct RangeAcquireResult {
    protocol_v2::Status status{protocol_v2::Status::InvalidState};
    std::size_t lines_requested{};
    std::size_t lines_granted{};
    bool complete{};
};

RangeAcquireResult acquireRange(std::uint64_t address, std::size_t size, RangeIntent intent);
```

The implementation validates overflow and alignment, iterates in 64-byte increments, and calls a private
`acquireLine(std::uint64_t line_address, RangeIntent intent)`. Read intent uses the existing GETS miss path without
copying data to an application buffer. Write intent uses the existing GETM/UPGRADE path without mutating line bytes.
It stops on the first failure and never reports `complete=true` unless every line is granted. It does not roll back
already acknowledged endpoint effects.

- [ ] **Step 4: Run focused and coherence tests**

```bash
cmake --build build --target test_coherence_range_acquire test_type2_coherence_litmus test_endpoint_completion_semantics -j8
ctest --test-dir build -R 'coherence_range|type2_coherence|endpoint_completion' --output-on-failure
```

Expected: all selected tests pass and the partial test reports one grant without a requester-wide success.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt include/coherence_endpoint_cache.h src/coherence_endpoint_cache.cpp tests/test_coherence_range_acquire.cpp
git commit -m "coherence: add fail-closed range acquisition"
```

### Task 3: Replace the QEMU coherent-pool RAM bypass with a trapping overlay

**Files:**
- Modify: `lib/qemu/include/hw/cxl/cxl_type2.h`
- Modify: `lib/qemu/hw/cxl/cxl_type2.c`
- Create: `lib/qemu/tests/qtest/cxl-type2-coherent-pool-test.c`
- Modify: `lib/qemu/tests/qtest/meson.build`

- [ ] **Step 1: Add qtests that expose the current bypass**

The qtest writes one value through the guest-visible pool, reads it back, and reads new debug counters exposed by the device:

```c
qtest_writel(qts, pool_hpa, 0x11223344);
g_assert_cmphex(qtest_readl(qts, pool_hpa), ==, 0x11223344);
g_assert_cmpuint(type2_debug_counter(qts, CXL_T2_DBG_HOST_STORES), ==, 1);
g_assert_cmpuint(type2_debug_counter(qts, CXL_T2_DBG_HOST_LOADS), ==, 1);
```

Also assert that an access crossing the pool end returns a device error and does not alter backing bytes.

- [ ] **Step 2: Run qtest and verify the counter assertion fails**

```bash
ninja -C build/qemu-vectordb cxl-type2-coherent-pool-test
build/qemu-vectordb/tests/qtest/cxl-type2-coherent-pool-test
```

Expected: the data round trip succeeds but host load/store counters remain zero because RAM bypasses callbacks.

- [ ] **Step 3: Split guest exposure from backing storage**

Add explicit fields:

```c
MemoryRegion coherent_pool_backing;
MemoryRegion coherent_pool_overlay;
uint8_t *coherent_pool_host_ptr;
uint64_t coherent_pool_host_loads;
uint64_t coherent_pool_host_stores;
```

Initialize `coherent_pool_backing` with `memory_region_init_ram()` but do not add it to BAR4. Add `coherent_pool_overlay` with `memory_region_init_io()` at the pool offset. Its read/write callbacks perform bounds checks, invoke the protocol-v2 host endpoint, and only then copy to/from `coherent_pool_host_ptr`. Remove the high-priority RAM subregion that currently bypasses coherency.

- [ ] **Step 4: Run qtest and existing CXL tests**

```bash
ninja -C build/qemu-vectordb qemu-system-x86_64 cxl-type2-coherent-pool-test
build/qemu-vectordb/tests/qtest/cxl-type2-coherent-pool-test
ninja -C build/qemu-vectordb test-qtest-x86_64
```

Expected: pool counter assertions pass and existing CXL qtests remain green.

- [ ] **Step 5: Commit QEMU change**

```bash
git -C lib/qemu add include/hw/cxl/cxl_type2.h hw/cxl/cxl_type2.c tests/qtest/cxl-type2-coherent-pool-test.c tests/qtest/meson.build
git -C lib/qemu commit -m "cxl/type2: trap coherent pool accesses"
```

### Task 4: Register the single Type-2 backing with the real GPU

**Files:**
- Modify: `lib/qemu/include/hw/cxl/cxl_hetgpu.h`
- Modify: `lib/qemu/hw/cxl/cxl_hetgpu.c`
- Modify: `lib/qemu/include/hw/cxl/cxl_type2.h`
- Modify: `lib/qemu/hw/cxl/cxl_type2.c`
- Modify: `lib/qemu/tests/qtest/cxl-type2-coherent-pool-test.c`

- [ ] **Step 1: Add backend tests with stubbed CUDA calls**

Test the registration lifecycle and exact pointer identity through the existing injectable/dynamic CUDA function table:

```c
g_assert_cmpint(hetgpu_register_coherent_region(&state, backing, size, &region), ==, HETGPU_SUCCESS);
g_assert_true(region.host_ptr == backing);
g_assert_cmpuint(region.size, ==, size);
g_assert_cmpuint(fake_cuda.host_register_calls, ==, 1);
g_assert_cmpuint(fake_cuda.host_get_device_pointer_calls, ==, 1);
hetgpu_unregister_coherent_region(&state, &region);
g_assert_cmpuint(fake_cuda.host_unregister_calls, ==, 1);
```

- [ ] **Step 2: Run the test and verify missing symbols fail compilation**

```bash
ninja -C build/qemu-vectordb cxl-type2-coherent-pool-test
```

Expected: compile failure for `hetgpu_register_coherent_region`.

- [ ] **Step 3: Implement CUDA host registration**

Load `cuMemHostRegister_v2`, `cuMemHostGetDevicePointer_v2`, and `cuMemHostUnregister`. Implement:

```c
HetGPUError hetgpu_register_coherent_region(HetGPUState *state, void *host_ptr,
                                             size_t size, HetGPUCoherentRegion *out);
HetGPUError hetgpu_unregister_coherent_region(HetGPUState *state,
                                               HetGPUCoherentRegion *region);
```

Use `CU_MEMHOSTREGISTER_DEVICEMAP`, fail when the backend is simulation or when `supports_coherent_memory` is false, and unregister exactly once during unrealize. QEMU realization fails when `gpu-mode=1` requested real-GPU coherent sharing but registration fails; it must not fall back to a shadow allocation.

- [ ] **Step 4: Re-run QEMU tests**

```bash
ninja -C build/qemu-vectordb qemu-system-x86_64 cxl-type2-coherent-pool-test
build/qemu-vectordb/tests/qtest/cxl-type2-coherent-pool-test
```

Expected: registration, pointer identity, error, and cleanup tests pass.

- [ ] **Step 5: Commit QEMU change**

```bash
git -C lib/qemu add include/hw/cxl/cxl_hetgpu.h hw/cxl/cxl_hetgpu.c include/hw/cxl/cxl_type2.h hw/cxl/cxl_type2.c tests/qtest/cxl-type2-coherent-pool-test.c
git -C lib/qemu commit -m "cxl/type2: map coherent backing into hetGPU"
```

### Task 5: Add the guest-to-QEMU range-acquire ABI

**Files:**
- Modify: `lib/qemu/include/hw/cxl/cxl_type2_gpu_cmd.h`
- Modify: `lib/qemu/hw/cxl/cxl_type2.c`
- Modify: `qemu_integration/guest_libcuda/cxl_gpu_cmd.h`
- Modify: `qemu_integration/guest_libcuda/libcuda.c`
- Modify: `lib/qemu/tests/qtest/cxl-type2-coherent-pool-test.c`

- [ ] **Step 1: Add ABI-negative tests**

Define test expectations for read acquire, write acquire, overflow, unaligned base, and injected failure:

```c
type2_cmd(qts, CXL_GPU_CMD_COH_ACQUIRE_RANGE, pool_off, 128, CXL_COH_RANGE_READ);
g_assert_cmpuint(type2_status(qts), ==, CXL_GPU_SUCCESS);
g_assert_cmpuint(type2_result(qts, 0), ==, 2); /* granted lines */
```

The injected second-line failure must return `CXL_GPU_ERROR_COHERENCY` with `result0=1`; kernel launch after that command remains blocked.

- [ ] **Step 2: Verify unknown opcode failure**

```bash
ninja -C build/qemu-vectordb cxl-type2-coherent-pool-test
build/qemu-vectordb/tests/qtest/cxl-type2-coherent-pool-test
```

Expected: range opcode returns invalid command.

- [ ] **Step 3: Implement the ABI and guest wrappers**

```c
enum {
    CXL_GPU_CMD_COH_ACQUIRE_RANGE = 0xB2,
    CXL_GPU_CMD_COH_RELEASE_RANGE = 0xB3,
};
enum { CXL_COH_RANGE_READ = 0, CXL_COH_RANGE_WRITE = 1 };

int cxlCoherentAcquireRange(void *host_ptr, uint64_t size, int intent,
                            uint64_t *device_ptr, uint64_t *lines_granted);
int cxlCoherentReleaseRange(void *host_ptr, uint64_t size, int dirty);
```

QEMU validates that the range belongs to one live coherent allocation, asks the device endpoint to acquire every line, returns the CUDA-mapped pointer plus offset only after complete success, and synchronizes before release. Partial acquisition sets a sticky launch-blocked status until a successful full acquire or reset.

- [ ] **Step 4: Run ABI and server coherence tests**

```bash
ninja -C build/qemu-vectordb qemu-system-x86_64 cxl-type2-coherent-pool-test
cmake --build build --target test_coherence_range_acquire test_type2_coherence_litmus -j8
ctest --test-dir build -R 'coherence_range|type2_coherence' --output-on-failure
```

Expected: all tests pass and no protocol-v1 fallback is logged.

- [ ] **Step 5: Commit both repositories**

```bash
git -C lib/qemu add include/hw/cxl/cxl_type2_gpu_cmd.h hw/cxl/cxl_type2.c tests/qtest/cxl-type2-coherent-pool-test.c
git -C lib/qemu commit -m "cxl/type2: add coherent range acquisition commands"
git add qemu_integration/guest_libcuda/cxl_gpu_cmd.h qemu_integration/guest_libcuda/libcuda.c
git commit -m "guest: expose Type-2 coherent range acquisition"
```

### Task 6: Implement software-CC dirty-range logic

**Files:**
- Create: `include/vectordb_dirty_ranges.h`
- Create: `src/vectordb_dirty_ranges.cpp`
- Create: `tests/test_vectordb_dirty_ranges.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing coalescing tests**

```cpp
DirtyRangeTracker tracker(64, 4096);
tracker.mark(64, 64);
tracker.mark(128, 128);
CHECK(tracker.coalesced() == std::vector<ByteRange>{{64, 192}});
CHECK(tracker.bytesToCopy() == 192);
tracker.clear();
CHECK(tracker.coalesced().empty());
```

Also reject zero-sized, overflowing, and out-of-allocation updates; round partial rows out to cache-line boundaries.

- [ ] **Step 2: Verify the missing implementation fails**

```bash
cmake --build build --target test_vectordb_dirty_ranges -j8
```

Expected: compile failure for `DirtyRangeTracker`.

- [ ] **Step 3: Implement the tracker**

Use a bit vector indexed by 64-byte line number. `coalesced()` scans once and emits maximal adjacent byte ranges. Keep flush execution out of this class so unit tests remain deterministic.

- [ ] **Step 4: Run focused tests**

```bash
cmake --build build --target test_vectordb_dirty_ranges -j8
ctest --test-dir build -R vectordb_dirty_ranges --output-on-failure
```

Expected: all dirty-range tests pass.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt include/vectordb_dirty_ranges.h src/vectordb_dirty_ranges.cpp tests/test_vectordb_dirty_ranges.cpp
git commit -m "vectordb: add software coherence dirty ranges"
```

### Task 7: Build the real-GPU VectorDB benchmark and oracle

**Files:**
- Create: `qemu_integration/guest_libcuda/vectordb_shared_index.c`
- Modify: `qemu_integration/guest_libcuda/Makefile`
- Create: `tests/test_vectordb_shared_index_contract.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add a contract test before benchmark code**

The test requires `--mode`, deterministic JSONL fields, stale-negative support, and no-copy HWCC accounting:

```c
g_assert_true(source_contains("schema\":\"splash.vectordb.v1"));
g_assert_true(source_contains("type2-hwcc"));
g_assert_true(source_contains("software-cc"));
g_assert_true(source_contains("negative-stale"));
g_assert_true(source_contains("copied_bytes"));
```

- [ ] **Step 2: Run the contract test and observe failure**

```bash
cmake --build build --target test_vectordb_shared_index_contract -j8
ctest --test-dir build -R vectordb_shared_index_contract --output-on-failure
```

Expected: failure because the benchmark source is absent.

- [ ] **Step 3: Implement one deterministic exact-L2 kernel path**

Embed PTX for a tiled float32 L2 kernel and deterministic top-10 reduction. Resolve the CXL extension functions with
`dlsym(RTLD_DEFAULT, ...)` so one source can be linked either to the guest shim or the system CUDA driver. Parse:

```text
--mode type2-hwcc|software-cc|full-copy|native-gpu|negative-stale
--rows N --dim 128 --queries N --topk 10 --update-ratio R
--warmup 5 --epochs 10 --seed N
```

For `type2-hwcc`, allocate once with `cxlCoherentAlloc`, update through the CPU pointer, call `cxlCoherentAcquireRange`, and pass only its mapped device pointer to `cuLaunchKernel`. Assert `copied_bytes==0`. For `software-cc`, flush dirty CPU lines with `_mm_clflushopt`, execute `_mm_sfence`, coalesce ranges, issue H2D copies, and count exact bytes. `full-copy` copies the full matrix. `negative-stale` skips propagation and must observe the targeted old label. Every positive epoch compares labels and distances with a CPU exact oracle.

- [ ] **Step 4: Build separate guest and native binaries, then run native/negative controls**

```bash
make -C qemu_integration/guest_libcuda vectordb_shared_index_guest vectordb_shared_index_native
qemu_integration/guest_libcuda/vectordb_shared_index_native --mode native-gpu --rows 4096 --queries 8 --epochs 1
qemu_integration/guest_libcuda/vectordb_shared_index_native --mode negative-stale --rows 4096 --queries 8 --epochs 1
```

Expected: native output has `correct=true`; negative output has `stale_observed=true`. Both identify the RTX PRO 6000 and never report a simulation backend.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt tests/test_vectordb_shared_index_contract.c qemu_integration/guest_libcuda/Makefile qemu_integration/guest_libcuda/vectordb_shared_index.c
git commit -m "vectordb: add CPU GPU shared-index benchmark"
```

### Task 8: Add the experiment runner and proof validator

**Files:**
- Create: `script/run_type2_vectordb.py`
- Create: `tests/test_run_type2_vectordb.py`

- [ ] **Step 1: Write validator-negative tests**

```python
def test_hwcc_rejects_zero_snoop_ack(tmp_path):
    row = valid_hwcc_row(snoop_acks=0)
    assert validate_row(row) == ["type2-hwcc requires post-update snoop ACKs"]

def test_hwcc_rejects_copied_bytes(tmp_path):
    row = valid_hwcc_row(copied_bytes=64)
    assert validate_row(row) == ["type2-hwcc copied_bytes must be zero"]
```

Also reject protocol v1, mock GPU, mismatched commits, missing negative-stale evidence, incorrect top-k, partial grants, and malformed JSON.

- [ ] **Step 2: Run tests and verify import failure**

```bash
python3 -m unittest tests.test_run_type2_vectordb -v
```

Expected: import failure for `script.run_type2_vectordb`.

- [ ] **Step 3: Implement orchestration and schema validation**

The runner supports `--smoke`, `--sweep`, `--validate-only`, and `--dry-run`. It creates a directory named from
`datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")` plus the runner PID under `artifact/type2_vectordb/`, records
machine inventory and commit IDs, launches the v2 server and QEMU with real hetGPU, installs/runs the guest benchmark
over SSH, parses JSONL, and writes only validated rows to `summary.csv`. Process cleanup uses bounded TERM/wait/KILL
logic inherited from `run_type2_coherence_litmus.sh`. The full sweep has an explicit eight-hour wall-clock budget.

- [ ] **Step 4: Run unit and dry-run tests**

```bash
python3 -m unittest tests.test_run_type2_vectordb -v
python3 script/run_type2_vectordb.py --dry-run --smoke
```

Expected: tests pass; dry-run prints protocol v2, host endpoint 0, device endpoint 1, `gpu-mode=1`, and no simulation fallback.

- [ ] **Step 5: Commit**

```bash
git add script/run_type2_vectordb.py tests/test_run_type2_vectordb.py
git commit -m "eval: add audited Type-2 VectorDB runner"
```

### Task 9: Run the smoke gate and paper sweep

**Files:**
- Create generated artifacts under: `artifact/type2_vectordb/`

- [ ] **Step 1: Run smoke with all compared modes**

```bash
python3 script/run_type2_vectordb.py --smoke
SMOKE_RUN=$(find artifact/type2_vectordb -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort | tail -1)
test -n "$SMOKE_RUN"
```

Expected: five modes complete; four positive modes have exact top-k; negative-stale observes an old label; HWCC has zero copied bytes and nonzero GETS, GETM/UPGRADE, and snoop ACK counters.

- [ ] **Step 2: Audit the smoke artifact before scaling**

```bash
python3 script/run_type2_vectordb.py --validate-only "artifact/type2_vectordb/$SMOKE_RUN"
```

Expected: `status=pass`, one physical backing identity in HWCC, real GPU identity, protocol v2, and no partial grants.

- [ ] **Step 3: Run the required paper subset**

```bash
python3 script/run_type2_vectordb.py --sweep --paper-subset
PAPER_RUN=$(find artifact/type2_vectordb -mindepth 1 -maxdepth 1 -type d -name '[0-9]*' -printf '%f\n' | sort | tail -1)
test -n "$PAPER_RUN"
```

Expected: size scaling at 0.1 percent updates, update-ratio scaling at 64K rows, and batch scaling at 64K rows each have ten measured epochs per point.

- [ ] **Step 4: Run the full sweep if the measured runtime is within the runner's recorded budget**

```bash
python3 script/run_type2_vectordb.py --sweep
```

Expected: the full Cartesian sweep finishes without failed gates. If it exceeds eight hours, preserve the paper subset
and record `full_sweep_status=budget_exceeded`; do not fabricate missing points.

### Task 10: Plot and update the paper from validated results

**Files:**
- Create: `script/plot_type2_vectordb.py`
- Create: `/home/victoryang00/CXLMemSim/697a667df59bdd0085f8ce88/figures/type2_vectordb_qps.pdf`
- Create: `/home/victoryang00/CXLMemSim/697a667df59bdd0085f8ce88/figures/type2_vectordb_sync.pdf`
- Modify: `/home/victoryang00/CXLMemSim/697a667df59bdd0085f8ce88/pact_draft_jesun.tex`

- [ ] **Step 1: Add plot input validation**

The plotter rejects unvalidated rows and requires all three primary modes at each x-coordinate:

```python
required = {"type2-hwcc", "software-cc", "full-copy"}
if set(group.mode) != required:
    raise ValueError(f"incomplete comparison at {group.key}")
```

- [ ] **Step 2: Generate publication figures**

```bash
PAPER_REPO=/home/victoryang00/CXLMemSim/697a667df59bdd0085f8ce88
PAPER_RUN=$(find artifact/type2_vectordb -mindepth 1 -maxdepth 1 -type d -name '[0-9]*' -printf '%f\n' | sort | tail -1)
test -n "$PAPER_RUN"
python3 script/plot_type2_vectordb.py "artifact/type2_vectordb/$PAPER_RUN/summary.csv" \
  --output-dir "$PAPER_REPO/figures"
```

Expected: one QPS figure with p25-p75 intervals and one synchronization time/bytes figure; labels distinguish functional Type-2 emulation from native GPU.

- [ ] **Step 3: Write only measured claims into the evaluation section**

Add an experimental-design paragraph, figure blocks, results with computed ratios, and this proof-boundary sentence:

```tex
The GPU executes the exact-search kernel on the physical RTX PRO 6000, while
\splash functionally models line ownership and snoop completion; the experiment
does not claim that the physical GPU link implements CXL.cache.
```

Do not retain placeholder numbers or describe unavailable sweep points.

- [ ] **Step 4: Build the paper and inspect warnings**

```bash
cd /home/victoryang00/CXLMemSim/697a667df59bdd0085f8ce88
latexmk -pdf -interaction=nonstopmode -halt-on-error pact_draft_jesun.tex
```

Expected: PDF builds with no undefined references to the new figures and no overfull boxes introduced by the new text.

- [ ] **Step 5: Commit paper and plotting changes in their owning repositories**

```bash
git add script/plot_type2_vectordb.py
git commit -m "eval: plot Type-2 VectorDB coherence results"
PAPER_REPO=/home/victoryang00/CXLMemSim/697a667df59bdd0085f8ce88
git -C "$PAPER_REPO" add pact_draft_jesun.tex figures/type2_vectordb_qps.pdf figures/type2_vectordb_sync.pdf
git -C "$PAPER_REPO" commit -m "Evaluation: add shared VectorDB index results"
```

### Task 11: Final verification, gitlink commit, and push

**Files:**
- Modify gitlink: `lib/qemu`

- [ ] **Step 1: Run the complete server and runner test suite**

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCXLMEMSIM_ENABLE_SLUGALLOCATOR=OFF
cmake --build build-release -j8
ctest --test-dir build-release --output-on-failure
python3 -m unittest tests.test_run_type2_vectordb -v
```

Expected: 100 percent pass.

- [ ] **Step 2: Run QEMU verification**

```bash
ninja -C build/qemu-vectordb qemu-system-x86_64
build/qemu-vectordb/tests/qtest/cxl-type2-coherent-pool-test
ninja -C build/qemu-vectordb test-qtest-x86_64
```

Expected: all QEMU Type-2 and CXL qtests pass.

- [ ] **Step 3: Revalidate the exact artifact used by the paper**

```bash
PAPER_REPO=/home/victoryang00/CXLMemSim/697a667df59bdd0085f8ce88
PAPER_RUN=$(find artifact/type2_vectordb -mindepth 1 -maxdepth 1 -type d -name '[0-9]*' -printf '%f\n' | sort | tail -1)
test -n "$PAPER_RUN"
python3 script/run_type2_vectordb.py --validate-only "artifact/type2_vectordb/$PAPER_RUN"
mkdir -p artifact/type2_vectordb/paper
install -m 0644 "artifact/type2_vectordb/$PAPER_RUN/summary.csv" artifact/type2_vectordb/paper/summary.csv
install -m 0644 "artifact/type2_vectordb/$PAPER_RUN/manifest.json" artifact/type2_vectordb/paper/manifest.json
sha256sum artifact/type2_vectordb/paper/summary.csv "$PAPER_REPO"/figures/type2_vectordb_*.pdf \
  > artifact/type2_vectordb/paper/SHA256SUMS
```

Expected: validation passes and hashes are stored in the artifact manifest.

- [ ] **Step 4: Commit the final QEMU gitlink and manifest**

```bash
git add lib/qemu artifact/type2_vectordb/paper/manifest.json artifact/type2_vectordb/paper/summary.csv \
  artifact/type2_vectordb/paper/SHA256SUMS
git commit -m "eval: record Type-2 VectorDB proof artifact"
```

- [ ] **Step 5: Push all three owning branches**

```bash
git -C lib/qemu push -u origin codex/type2-vectordb-shared-index-20260810
git push -u origin codex/type2-vectordb-shared-index-20260810
git -C /home/victoryang00/CXLMemSim/697a667df59bdd0085f8ce88 push origin master
```

Expected: each push reports its recorded commit and the superproject gitlink points to the pushed QEMU commit.
