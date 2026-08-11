# PGAS Exit Statistics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Emit one accurate, machine-readable final statistics snapshot for PGAS_SHM on normal exit, SIGINT, SIGTERM, and repeated `stop()` calls.

**Architecture:** Add a black-box PGAS client fixture and process-level regression test before changing the server. Separate idempotent cleanup from a once-only statistics snapshot in `ThreadPerConnectionServer`; serialize the same snapshot to log text and optional JSON.

**Tech Stack:** C++20, C11 PGAS backend API, CMake/CTest, Python 3 `unittest`, spdlog, POSIX signals and shared memory.

---

## File Structure

- Create `tests/pgas_exit_stats_client.c`: issue one READ, WRITE, FAA, CAS, and FENCE request through `cxl_backend.h`.
- Create `tests/test_server_exit_stats.py`: launch the real server, run the client, signal it, and validate text/JSON counters.
- Modify `src/main_server.cc`: parse `--stats-json`, snapshot counters, print once, and write JSON atomically.
- Modify `CMakeLists.txt`: build the fixture and register the Python integration test.

### Task 1: Establish the failing process-level regression

**Files:**
- Create: `tests/pgas_exit_stats_client.c`
- Create: `tests/test_server_exit_stats.py`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add a PGAS client that exercises every required operation**

Map the named PGAS object using the protocol structures from `cxl_backend.h`,
claim slot zero, and fail on any response error:

```c
int fd = shm_open(argv[1], O_RDWR | O_CLOEXEC, 0);
cxl_shm_header_t *header = map_and_validate_header(fd);
cxl_shm_slot_t *slot = &header->slots[0];
uint64_t value = 7;
submit(slot, CXL_SHM_REQ_WRITE, 0, &value, sizeof(value), 0, 0);
submit(slot, CXL_SHM_REQ_READ, 0, NULL, sizeof(value), 0, 0);
submit(slot, CXL_SHM_REQ_ATOMIC_FAA, 0, NULL, sizeof(value), 1, 0);
submit(slot, CXL_SHM_REQ_ATOMIC_CAS, 0, NULL, sizeof(value), 9, 8);
submit(slot, CXL_SHM_REQ_FENCE, 0, NULL, 0, 0, 0);
```

`submit()` fills address, size, value, expected, timestamp, and optional data;
publishes `req_type` with release ordering; waits with a 10-second monotonic
deadline; requires `CXL_SHM_RESP_OK`; then clears `resp_status`.

- [ ] **Step 2: Add a process-level Python test**

The test must use a unique POSIX SHM name, wait for `server_ready`, run the client, send SIGTERM, and parse both output forms:

```python
server = subprocess.Popen(
    [server_bin, "--comm-mode=pgas-shm", f"--pgas-shm-name={shm_name}",
     "--capacity=16", "--backing-mode=file", f"--backing-file={backing_file}",
     f"--stats-json={stats_json}"],
    stdout=log, stderr=subprocess.STDOUT, text=True,
)
wait_for_text(log_path, "PGAS shared memory initialized", timeout=10)
subprocess.run([client_bin, shm_name], check=True, timeout=10)
server.send_signal(signal.SIGTERM)
assert server.wait(timeout=10) == 0
```

Assert exactly one `Final Server Statistics:` marker and JSON values
`reads=1`, `writes=1`, `atomic_faa=1`, `atomic_cas=1`,
`atomic_cas_success=1`, and `fences=1`.
Also require `controller.remote >= 4`, switch/endpoint load and store counters
to be nonzero, and `threads_created >= 1`.

The temporary regular-file backing is mandatory: the test must never open,
resize, or unlink the fixed `/dev/shm/cxlmemsim_shared` object used by another
server. Cleanup is limited to the unique test-owned PGAS SHM name.

- [ ] **Step 3: Register the fixture and test in CMake**

```cmake
add_executable(pgas_exit_stats_client tests/pgas_exit_stats_client.c)
target_include_directories(pgas_exit_stats_client PRIVATE include)
target_link_libraries(pgas_exit_stats_client ${RT_LIB} ${ATOMIC_LIB})

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_server_exit_stats
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_server_exit_stats.py
            --server $<TARGET_FILE:cxlmemsim_server>
            --client $<TARGET_FILE:pgas_exit_stats_client>)
```

- [ ] **Step 4: Run the test and preserve the negative result**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target cxlmemsim_server pgas_exit_stats_client
ctest --test-dir build -R test_server_exit_stats --output-on-failure
```

Expected before the production fix: FAIL because `--stats-json` is unknown or
the final marker/JSON snapshot is missing. Save stdout as
`artifact/pgas_exit_stats/negative-before-fix.log`.

- [ ] **Step 5: Commit the negative test**

```bash
git add CMakeLists.txt tests/pgas_exit_stats_client.c tests/test_server_exit_stats.py \
        artifact/pgas_exit_stats/negative-before-fix.log
git commit -s -m "test/server: pin PGAS exit statistics contract"
```

### Task 2: Implement a once-only final statistics snapshot

**Files:**
- Modify: `src/main_server.cc`
- Test: `tests/test_server_exit_stats.py`

- [ ] **Step 1: Add CLI configuration and once-only state**

Add `std::string stats_json_path` to option parsing, pass it to the server
constructor, and add:

```cpp
std::string stats_json_path_;
std::once_flag final_stats_once_;

void print_final_stats_once();
void write_final_stats_json(const std::string &json) const;
```

The CLI option is optional; an empty path disables JSON without changing text
statistics.

- [ ] **Step 2: Split cleanup from reporting**

Keep cleanup guarded by `running.exchange(false)`, but never return before the
once-only reporter:

```cpp
void ThreadPerConnectionServer::stop() {
    const bool perform_cleanup = running.exchange(false);
    if (perform_cleanup) {
        stop_listeners_and_join_workers();
    }
    print_final_stats_once();
}
```

If extracting `stop_listeners_and_join_workers()` would enlarge the change,
retain the current cleanup block inside `if (perform_cleanup)` and move only
the reporting block below it.

- [ ] **Step 3: Snapshot all counters once**

Inside `std::call_once`, load server atomics, MESI transitions/audit counters,
and controller topology counters into local values before formatting. Begin
the human output with the stable marker:

```cpp
std::call_once(final_stats_once_, [this] {
    const uint64_t reads = total_reads.load();
    const uint64_t writes = total_writes.load();
    SPDLOG_INFO("Final Server Statistics:");
    SPDLOG_INFO("  Total Reads: {}", reads);
    // Print the remaining snapshot values from the same locals.
});
```

Do not read mutable counters again while creating JSON.

- [ ] **Step 4: Write JSON atomically**

Serialize integers and mode metadata to `<path>.tmp.<pid>`, call `fsync`, close,
and `rename` to the requested path. The top-level schema is:

```json
{
  "schema": "cxlmemsim.server-stats.v1",
  "communication_mode": "pgas-shm",
  "server": {"reads": 1, "writes": 1, "atomic_faa": 1, "atomic_cas": 1,
             "atomic_cas_success": 1, "fences": 1},
  "controller": {"remote": 4, "local": 0, "hitm": 0, "threads_created": 1},
  "switches": [{"id": 0, "loads": 1, "stores": 3}],
  "endpoints": [{"id": 0, "loads": 1, "stores": 3}]
}
```

JSON write failure sets `final_stats_ok_ = false`. Expose
`final_stats_ok() const`; after `server.stop()`, `main()` returns one when the
requested JSON artifact could not be committed. It must not silently report
success with a missing audit artifact.

- [ ] **Step 5: Correct PGAS CAS success accounting**

Preserve the requested expected value before compare/exchange and increment
`total_atomic_cas_success` exactly when the returned old value equals that
expected value. The process test's `8 -> 9` CAS must report one success.

- [ ] **Step 6: Make exception paths report when initialization completed**

After `server.start()` succeeds, use a scope guard or explicit catch-path call
so `server.stop()` runs on normal return and exceptions. Do not print a final
snapshot for failures before server initialization.

- [ ] **Step 7: Run the focused regression**

Run:

```bash
cmake --build build -j --target cxlmemsim_server pgas_exit_stats_client
ctest --test-dir build -R 'test_(server_exit_stats|pgas_controller_counters)' --output-on-failure
```

Expected: both tests pass; the integration log has one final marker and the
JSON/text counters agree.

- [ ] **Step 8: Commit the fix**

```bash
git add src/main_server.cc tests/test_server_exit_stats.py
git commit -s -m "server: emit PGAS final statistics exactly once"
```

### Task 3: Complete regression verification

**Files:**
- No production changes expected.

- [ ] **Step 1: Run Release verification**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 2: Run Debug verification**

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j --target cxlmemsim_server pgas_exit_stats_client
ctest --test-dir build-debug -R 'test_(server_exit_stats|pgas_controller_counters)' --output-on-failure
```

Expected: focused tests pass without assertions or sanitizer-style runtime
diagnostics.

- [ ] **Step 3: Commit any test-only corrections and record evidence**

```bash
git status --short
git log -3 --oneline
```

Do not commit build products. Record exact commands and counts in the final
hardware artifact manifest created by the companion plan.
