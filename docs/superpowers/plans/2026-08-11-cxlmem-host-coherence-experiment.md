# Real CXL.mem Host-Coherence Experiment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce independently validated real-hardware evidence that CPU caches coherently access physical CXL.mem without flushes, then integrate only supported claims into the paper.

**Architecture:** A small C11 benchmark exposes isolated latency, message-passing, ownership-handoff, and atomic subcommands. A Python runner attests the device before writes, invokes each subcommand under supported PMUs, validates proof gates, and emits an immutable audit bundle consumed by the paper.

**Tech Stack:** C11 atomics, pthread CPU affinity, Linux devdax, x86 RDTSCP/CLFLUSH for cold-only controls, Linux perf, Python 3 `unittest`, CSV/JSON/SHA-256, LaTeX/latexmk.

---

## File Structure

- Create `microbench/cxlmem_host_cc_validation.c`: measurement-only executable with JSONL output.
- Modify `microbench/CMakeLists.txt`: build the executable with pthread and GNU11.
- Create `script/run_cxlmem_hwcc_validation.py`: attestation, execution, PMU collection, validation, and artifact writer.
- Create `tests/test_run_cxlmem_hwcc_validation.py`: pure unit tests for attestation, parser, proof gates, and validation-only mode.
- Create `script/plot_cxlmem_hwcc_validation.py`: deterministic paper table/figure generation.
- Modify `pact_draft_jesun.tex` in the paper worktree after the hardware artifact passes.
- Add a bounded audit copy under the paper tree.

### Task 1: Pin fail-closed attestation and artifact validation

**Files:**
- Create: `tests/test_run_cxlmem_hwcc_validation.py`
- Create: `script/run_cxlmem_hwcc_validation.py`

- [ ] **Step 1: Write failing attestation tests**

Build fixture sysfs trees and require rejection of every safety mismatch:

```python
def test_attestation_rejects_mapping_beyond_two_mib(self):
    with tempfile.TemporaryDirectory() as root:
        paths = write_hardware_fixture(Path(root))
        with self.assertRaisesRegex(ValidationError, "approved 2 MiB"):
            attest_hardware(paths, offset=0, length=2 * 1024**2 + 1)
```

Include BDF, PCI ID, serial, DVSEC `Cache- Mem+`, region mode/resource/size,
DAX mode/size, offset, open-holder, and advisory-lock cases.

- [ ] **Step 2: Implement read-only attestation**

Define immutable records and keep device opening separate:

```python
@dataclass(frozen=True)
class Attestation:
    bdf: str
    pci_id: str
    serial: str
    firmware: str
    region_resource: int
    region_size: int
    dax_size: int
    offset: int
    length: int
    checks: tuple[str, ...]

def attest_hardware(paths: HardwarePaths, offset: int, length: int) -> Attestation:
    if offset != 0 or length <= 0 or length > 2 * 1024**2:
        raise ValidationError("mapping exceeds approved 2 MiB boundary")
    # Validate all identity and mode fields before returning.
```

No test or validation-only code may open `/dev/dax0.0`.

- [ ] **Step 3: Add proof-gate validation fixtures**

Create a complete passing fixture and mutate one field per test. Require:

```python
assert summary["litmus"]["stale"] == 0
assert summary["atomic"]["ticket_errors"] == 0
assert summary["pmu"]["cold_cxl_reads"] > 0
assert summary["pmu"]["cache_to_cache_events"] > 0
assert summary["safety"]["max_written_offset"] <= 2 * 1024**2
```

Reject missing raw evidence, unsupported-only cache-to-cache PMUs, command
failures, duplicate result keys, checksum mismatches, and git commit mismatch.

- [ ] **Step 4: Implement `--validate-only` and deterministic checksums**

The validator reads an existing run directory, recomputes `results.csv` and
`summary.json`, checks SHA-256 entries, and writes only `validation.json`.
Expected terminal output is one JSON object containing `run_dir` and
`status=pass` or a nonzero exit with all validation errors.

- [ ] **Step 5: Run unit tests and commit**

```bash
python3 -m unittest tests.test_run_cxlmem_hwcc_validation -v
git add script/run_cxlmem_hwcc_validation.py tests/test_run_cxlmem_hwcc_validation.py
git commit -s -m "eval: define real CXL.mem validation contract"
```

Expected: unit tests pass without accessing hardware.

### Task 2: Implement the bounded C11 hardware benchmark

**Files:**
- Create: `microbench/cxlmem_host_cc_validation.c`
- Modify: `microbench/CMakeLists.txt`

- [ ] **Step 1: Add parser smoke tests to the Python suite**

Require one JSON object with schema `splash.cxlmem-hwcc.v1`, a known
subcommand, backend `dram` or `cxlmem`, CPU IDs, iteration count, errors, and
timing fields. Reject extra stdout before or after JSON.

- [ ] **Step 2: Implement bounded mapping and CPU pinning**

The executable accepts:

```text
--mode warm-load|cold-load|message-passing|handoff|fetch-add|cas
--backend dram|cxlmem --device /dev/dax0.0 --offset 0 --length 2097152
--cpu-a N --cpu-b N --iterations N
```

For CXL.mem, require `offset == 0`, `length <= 2097152`, use
`mmap(..., length, ..., MAP_SHARED, fd, offset)`, and never map the full 128 GiB
device. Use `pthread_setaffinity_np` and reject affinity failures.

- [ ] **Step 3: Implement warm and cold dependent loads**

Initialize a cache-line-stride pointer cycle within 2 MiB. Warm mode traverses
without flushes. Cold mode executes `_mm_clflush` on benchmark lines followed
by `_mm_mfence` before the timed traversal. Emit p50, p95, p99, average, and
operation count; do not describe the cold-only flush as software coherence.

- [ ] **Step 4: Implement no-flush message passing and handoff**

Use separate aligned atomic flag and payload lines:

```c
atomic_store_explicit(&state->payload, seq, memory_order_relaxed);
atomic_store_explicit(&state->flag, seq, memory_order_release);
while (atomic_load_explicit(&state->flag, memory_order_acquire) != seq) {
    _mm_pause();
}
if (atomic_load_explicit(&state->payload, memory_order_relaxed) != seq) {
    errors++;
}
```

For handoff, alternate an atomic turn token and verify every monotonic payload.
Add a watchdog deadline and emit `flushes_in_hot_path: 0`.

- [ ] **Step 5: Implement atomic ticket and CAS checks**

Fetch-add records every returned ticket in a per-thread array, sorts after
join, and requires exactly `0..2*iterations-1`. CAS uses an expected-value loop
and reports attempts, successes, failures, and exact final value.

- [ ] **Step 6: Build and run DRAM smoke tests only**

```bash
cmake --build build -j --target cxlmem_host_cc_validation
./build/microbench/cxlmem_host_cc_validation --mode message-passing \
  --backend dram --length 2097152 --cpu-a 0 --cpu-b 1 --iterations 100000
./build/microbench/cxlmem_host_cc_validation --mode fetch-add \
  --backend dram --length 2097152 --cpu-a 0 --cpu-b 43 --iterations 100000
```

Expected: one valid JSON object per command, zero errors, exact final values,
and zero hot-path flushes.

- [ ] **Step 7: Commit the benchmark**

```bash
git add microbench/CMakeLists.txt microbench/cxlmem_host_cc_validation.c \
        tests/test_run_cxlmem_hwcc_validation.py
git commit -s -m "microbench: add CXL.mem host-coherence litmus"
```

### Task 3: Add PMU discovery and experiment orchestration

**Files:**
- Modify: `script/run_cxlmem_hwcc_validation.py`
- Modify: `tests/test_run_cxlmem_hwcc_validation.py`

- [ ] **Step 1: Add tests for perf discovery and CSV parsing**

Parse `perf list` and choose supported events from ordered candidate groups:

```python
PMU_GROUPS = {
    "cxl_source": ("mem_load_retired.local_cxl_mem", "ocr.demand_data_rd.local_cxl_mem"),
    "same_socket_handoff": ("mem_load_l3_hit_retired.xsnp_fwd", "ocr.demand_data_rd.l3_hit.snoop_hitm"),
    "remote_handoff": ("mem_load_l3_miss_retired.remote_hitm", "ocr.demand_data_rd.remote_cache.snoop_hitm"),
    "device_reads": ("cxl_pmu_mem0.0/m2s_req_memrd/", "cxl_pmu_mem0.0/s2m_drs_memdata/"),
}
```

Tests must handle `<not supported>`, `<not counted>`, comma separators, and
event aliases.

- [ ] **Step 2: Implement topology-aware CPU selection**

Read `/sys/devices/system/cpu/cpu*/topology/{physical_package_id,core_id}`.
Select two distinct physical cores on package 0 and one core on package 1.
Record exact selections; fail if only one package or fewer than two physical
cores are online.

- [ ] **Step 3: Implement exclusive acquisition and live holder check**

Immediately before hardware execution, enumerate `/proc/[0-9]*/fd` links to
the DAX device, then open it and take `flock(LOCK_EX | LOCK_NB)`. Pass the
already opened descriptor to the benchmark through `/proc/self/fd/N` with
`pass_fds`, eliminating the check/open race. Any holder aborts before stores.

- [ ] **Step 4: Run each measurement under perf**

Use `perf stat -x, -o RAW --event EVENT... -- BENCHMARK ...` for five measured
repetitions of DRAM/CXL warm/cold and same/cross-socket litmus, handoff, FAA,
and CAS. Capture stdout/stderr and return codes. A failed or timed-out command
invalidates the run.

- [ ] **Step 5: Normalize and validate the run**

Write rows keyed by backend, mode, placement, repetition, and event. Enforce
zero correctness errors, exact atomic state, nonzero cold CXL source/media
events, and at least one nonzero cache-to-cache handoff event. Require warm
CXL media requests per operation to be lower than cold CXL media requests per
operation.

- [ ] **Step 6: Run the non-destructive preflight**

```bash
python3 script/run_cxlmem_hwcc_validation.py --preflight-only \
  --device /dev/dax0.0 --offset 0 --length 2097152
```

Expected: `status=pass`, exact BDF/serial/DVSEC, selected CPUs, supported PMUs,
and `device_opened=false`, `bytes_written=0`.

- [ ] **Step 7: Commit orchestration**

```bash
git add script/run_cxlmem_hwcc_validation.py tests/test_run_cxlmem_hwcc_validation.py
git commit -s -m "eval: orchestrate CXL.mem PMU validation"
```

### Task 4: Execute the approved 2 MiB hardware experiment

**Files:**
- Create a timestamped runtime artifact under `artifact/cxlmem_hwcc/`.

- [ ] **Step 1: Recheck the destructive boundary live**

Run `cxl list`, `daxctl list`, `/proc/*/fd` holder inspection, and preflight
again. Record that the only approved range is `[0, 0x200000)`.

- [ ] **Step 2: Execute the full run**

```bash
sudo -n python3 script/run_cxlmem_hwcc_validation.py \
  --device /dev/dax0.0 --offset 0 --length 2097152 \
  --repetitions 5 --iterations 1000000
RUN_DIR=$(find artifact/cxlmem_hwcc -mindepth 1 -maxdepth 1 -type d \
  -printf '%T@ %p\n' | sort -n | tail -1 | cut -d' ' -f2-)
test -n "$RUN_DIR"
```

Do not retry a hardware mutation after a kernel error, machine check, PCI AER
change, or process crash. Capture `dmesg`/AER evidence and stop.

- [ ] **Step 3: Independently validate without device access**

```bash
python3 script/run_cxlmem_hwcc_validation.py --validate-only "$RUN_DIR"
```

Expected: JSON output with `status=pass`. If PMU proof is absent, retain the
artifact as `blocked` and do not write a positive hardware claim.

- [ ] **Step 4: Commit the bounded audit artifact**

Commit manifest, normalized CSV, summary, validation, topology text, raw perf
CSV, and checksums. Exclude binaries, DAX dumps, and files larger than required
for audit.

```bash
git add "$RUN_DIR"
git commit -s -m "eval: add audited CXL.mem coherence results"
```

### Task 5: Generate deterministic paper material

**Files:**
- Create: `script/plot_cxlmem_hwcc_validation.py`
- Create: `tests/test_plot_cxlmem_hwcc_validation.py`
- Create: `$RUN_DIR/paper/host_coherence.pdf`
- Create: `$RUN_DIR/paper/host_coherence.csv`

- [ ] **Step 1: Test aggregation and deterministic output**

Require median and p25/p75 grouped by backend/mode/placement. Reject a source
run unless `validation.json` is `pass`. Assert stable CSV bytes and stable plot
data ordering.

- [ ] **Step 2: Implement a compact two-panel figure**

Panel (a) reports DRAM/CXL warm/cold and ownership-handoff latency with p25-p75.
Panel (b) reports normalized PMU events per operation for cold CXL and
same/cross-socket handoff. Include correctness counts in the companion CSV,
not as decorative chart text.

- [ ] **Step 3: Regenerate twice and compare**

```bash
python3 script/plot_cxlmem_hwcc_validation.py RUN_DIR
sha256sum RUN_DIR/paper/host_coherence.csv RUN_DIR/paper/host_coherence.pdf
python3 script/plot_cxlmem_hwcc_validation.py RUN_DIR
```

Expected: CSV is byte-identical; PDF plot data and dimensions are identical.

- [ ] **Step 4: Commit figure tooling and output**

```bash
git add script/plot_cxlmem_hwcc_validation.py tests/test_plot_cxlmem_hwcc_validation.py \
        "$RUN_DIR/paper"
git commit -s -m "eval: plot real CXL.mem coherence evidence"
```

### Task 6: Integrate validated results into the paper

**Files:**
- Modify in paper worktree: `pact_draft_jesun.tex`
- Add in paper worktree: `figures/host_coherence.pdf`
- Add in paper worktree: `artifact/cxlmem_hwcc/$RUN_ID/...` bounded audit copy,
  where `RUN_ID=$(basename "$RUN_DIR")`.

- [ ] **Step 1: Copy only independently validated material**

Verify source checksums, then copy the figure, paper CSV, manifest, summary,
validation, raw result/perf CSV, topology, and checksum file into the paper
worktree. Record the CXLMemSim commit in the paper artifact README.

- [ ] **Step 2: Correct terminology before adding results**

Replace ambiguous `Type-2 HWCC` prose and captions with `emulated Type-2
HWCC`. Replace the statement that CXLMemSim/LogP calibration validates
coherence with:

```tex
LogP parameterizes transport latency and queuing; it does not validate cache
coherence, ownership transfer, or snoop correctness.
```

- [ ] **Step 3: Add the real-hardware subsection**

State the Montage identity, `Cache- Mem+` DVSEC, 128 GiB devdax mapping, CPU
placement, five repetitions, and exact PMU events. Report only measured values
from `summary.json`. Explain that zero litmus/atomic errors plus CXL-source and
HITM/FWD counters jointly establish the host-facing decomposition claim.

- [ ] **Step 4: Add the explicit limitation**

State that the installed hardware cannot validate device-initiated
`CXL.cache`, DCOH capacity/replacement, bias transitions, or coherent GPU
access; a `Cache+ Mem+` Type-2 FPGA remains required for those claims.

- [ ] **Step 5: Build and visually inspect the paper**

```bash
latexmk -pdf -interaction=nonstopmode -halt-on-error pact_draft_jesun.tex
rm -f /tmp/cxlmem-paper-*.png
pdftoppm -png -r 150 pact_draft_jesun.pdf /tmp/cxlmem-paper
```

Inspect the affected pages for clipping, illegible labels, floating figures,
and incorrect references. Run `rg 'undefined references|Citation.*undefined'`
on the log and distinguish pre-existing warnings.

- [ ] **Step 6: Commit and push the paper**

```bash
git add pact_draft_jesun.tex figures/host_coherence.pdf artifact/cxlmem_hwcc
git commit -s -m "paper: add real CXL.mem coherence validation"
git push origin HEAD:master
```

Expected: Overleaf `master` resolves to the new commit.

### Task 7: Final verification and push

**Files:**
- No new files expected.

- [ ] **Step 1: Run all focused and repository tests**

```bash
python3 -m unittest tests.test_run_cxlmem_hwcc_validation tests.test_plot_cxlmem_hwcc_validation -v
ctest --test-dir build --output-on-failure
python3 script/run_cxlmem_hwcc_validation.py --validate-only RUN_DIR
```

- [ ] **Step 2: Verify commit provenance and clean tracked state**

Check superproject HEAD, remote branch, QEMU gitlink, paper HEAD, Overleaf
master, artifact checksums, and that only expected generated binaries/cache
files remain untracked.

- [ ] **Step 3: Push the implementation branch**

```bash
git push -u origin codex/cxlmem-hwcc-validation-20260811
```

Report exact commits, run directory, test counts, measured results, and the
remaining Type-2 hardware proof boundary.
