# PGAS SHM Fast-Poll Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce synchronous PGAS SHM request wakeup overhead while keeping every guest CXL load mediated by the memory server.

**Architecture:** A testable server-side policy selects spin, yield, or sleep from recent activity. QEMU uses a small pure-C wait policy to spin during the expected response window and sleep only on cold or failed paths; the existing slot protocol and timeouts remain authoritative.

**Tech Stack:** C++20, C11, CMake/CTest, QEMU C and Meson build, POSIX shared memory, Linux KVM.

**Spec:** `docs/superpowers/specs/2026-08-20-pgas-fast-poll-design.md`

## Global Constraints

- Preserve a synchronous memory-server request and response for every guest CXL access.
- Do not add direct-mapped, asynchronous-accounting, or request-batching paths.
- Keep bounded client timeouts and cold-path sleeps.
- Validate with the exact 128 MiB `lat_mem_rd` command and `/dev/dax0.0` mapping.

---

### Task 1: Adaptive memory-server polling

**Files:**
- Create: `include/pgas_poll_policy.h`
- Create: `tests/test_pgas_poll_policy.cpp`
- Modify: `src/main_server.cc`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `PgasPollPolicy::next(bool processed, std::uint64_t now_ns) -> PgasPollAction`.
- Produces: server options `pgas_workers`, `pgas_spin_us`, `pgas_yield_count`, and `pgas_idle_sleep_us`.

- [ ] **Step 1: Write the failing policy test**

```cpp
PgasPollPolicy policy(50'000, 2);
assert(policy.next(true, 1'000'000) == PgasPollAction::Spin);
assert(policy.next(false, 1'049'999) == PgasPollAction::Spin);
assert(policy.next(false, 1'050'000) == PgasPollAction::Yield);
assert(policy.next(false, 1'050'001) == PgasPollAction::Yield);
assert(policy.next(false, 1'050'002) == PgasPollAction::Sleep);
```

- [ ] **Step 2: Run the focused build and verify RED**

Run: `cmake --build build --target test_pgas_poll_policy -j$(nproc)`

Expected: fail because the target and `pgas_poll_policy.h` do not exist.

- [ ] **Step 3: Implement the policy and server options**

Implement a header-only policy storing the last-active timestamp and yield
count. Parse and validate the four server options, log their resolved values,
and apply the selected action in each PGAS worker loop.

- [ ] **Step 4: Run focused and full tests**

Run: `cmake --build build --target test_pgas_poll_policy cxlmemsim_server -j$(nproc) && ctest --test-dir build --output-on-failure`

Expected: the policy test and all existing CTest cases pass.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt include/pgas_poll_policy.h tests/test_pgas_poll_policy.cpp src/main_server.cc
git commit -m "server: add adaptive PGAS polling"
```

### Task 2: Adaptive QEMU PGAS response wait

**Files:**
- Create: `lib/qemu/include/hw/cxl/cxl_memsim_wait.h`
- Modify: `lib/qemu/hw/mem/cxl_type3.c`
- Create: `tests/test_cxl_memsim_wait_policy.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `cxl_memsim_wait_action(uint64_t elapsed_ns, uint64_t spin_ns) -> CXLMemSimWaitAction`.
- Consumes: `CXL_PGAS_CLIENT_SPIN_US` and `CXL_PGAS_CLIENT_SLEEP_US` environment variables.

- [ ] **Step 1: Write the failing QEMU wait-policy test**

```c
assert(cxl_memsim_wait_action(49'999, 50'000) == CXL_MEMSIM_WAIT_SPIN);
assert(cxl_memsim_wait_action(50'000, 50'000) == CXL_MEMSIM_WAIT_SLEEP);
```

Use ordinary decimal literals in the C source (`49999` and `50000`).

- [ ] **Step 2: Run the focused build and verify RED**

Run: `cmake --build build --target test_cxl_memsim_wait_policy -j$(nproc)`

Expected: fail because the target and helper header do not exist.

- [ ] **Step 3: Implement and integrate the wait helper**

Implement the pure-C boundary helper, parse the two environment settings during
CXLMemSim initialization, and replace the slot-free and response fixed-sleep
loops with monotonic deadline loops using `cpu_relax()` during the hot window.
Accumulate response-wait count, total nanoseconds, and maximum nanoseconds and
log them every 100,000 successful requests.

- [ ] **Step 4: Run focused tests and build QEMU**

Run: `cmake --build build --target test_cxl_memsim_wait_policy -j$(nproc)`

Run: `cmake --build build -j$(nproc)`

Run the existing QEMU configure/build command discovered from the active build,
targeting the isolated QEMU checkout.

Expected: both wait-policy tests and both projects compile successfully.

- [ ] **Step 5: Commit both repository layers**

```bash
git -C lib/qemu add include/hw/cxl/cxl_memsim_wait.h hw/mem/cxl_type3.c
git -C lib/qemu commit -m "cxl: busy-poll PGAS responses on the hot path"
git add lib/qemu CMakeLists.txt tests/test_cxl_memsim_wait_policy.c
git commit -m "qemu: test PGAS wait policy"
```

### Task 3: Live latency validation

**Files:**
- Create: `artifact/pgas_fast_poll/<timestamp>/server-command.txt`
- Create: `artifact/pgas_fast_poll/<timestamp>/qemu-environment.txt`
- Create: `artifact/pgas_fast_poll/<timestamp>/lat_mem_rd-128m.txt`
- Create: `artifact/pgas_fast_poll/<timestamp>/dax-mapping.txt`
- Create: `artifact/pgas_fast_poll/<timestamp>/kernel-traps.txt`
- Create: `artifact/pgas_fast_poll/<timestamp>/summary.md`

**Interfaces:**
- Consumes: isolated server and QEMU builds from Tasks 1 and 2.
- Produces: reproducible before/after wall-clock latency evidence.

- [ ] **Step 1: Capture and stop the current launch cleanly**

Record the active server command, QEMU command/environment, guest process state,
and current kernel trap tail before stopping only the identified server and QEMU
processes.

- [ ] **Step 2: Launch the optimized synchronous path**

Start the isolated server with:

```bash
./build/cxlmemsim_server --comm-mode=pgas-shm \
  --pgas-shm-name=/cxlmemsim_pgas --capacity=256 \
  --pgas-workers=1 --pgas-spin-us=50 --pgas-yield-count=10 \
  --pgas-idle-sleep-us=100
```

Launch the isolated QEMU build with `CXL_PGAS_CLIENT_SPIN_US=50` and
`CXL_PGAS_CLIENT_SLEEP_US=10`, preserving all other active launch arguments.

- [ ] **Step 3: Run the exact guest benchmark**

Run:

```bash
LD_PRELOAD=target/debug/libcxlalloc_preload.so \
  ~/lmbench/bin/x86_64-linux-gnu/lat_mem_rd -t -N 4 128 64
```

Capture the complete output, exit status, live `/proc/<pid>/maps` entry for
`/dev/dax0.0`, and kernel trap lines.

- [ ] **Step 4: Compare and verify**

Compare the 128 MiB result with the accepted approximately 4130 ns/load
baseline. Verify that the command exits zero, the DAX mapping is present, the
server request counter increases, and no new invalid-opcode trap is recorded.

- [ ] **Step 5: Commit evidence**

```bash
git add artifact/pgas_fast_poll
git commit -m "bench: record PGAS fast-poll latency"
```
