# Repository Guidelines

## Project Structure & Modules
- Source: `src/` (core C++: controller, server, helpers), headers in `include/`.
- Microbenchmarks: `microbench/` (C/C++ workloads and utilities).
- Integration: `qemu_integration/`, optional libs in `lib/` (e.g., qemu, bpftime).
- Scripts & demos: top-level `run_demo.sh`, `run_protocol_demo.sh`.
- Build artifacts: `build/` (CMake), logs in repo root, shared memory at `/dev/shm/cxlmemsim_shared`.

## Build, Test, and Development
- Configure and build:
  - `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
  - `cmake --build build -j` (targets include `cxlmemsim_server`, microbench apps)
- Dependencies: CMake ≥ 3.11, GCC/Clang, `libspdlog-dev`, `libcxxopts-dev`, Linux headers (for BPF pieces).
- Run server:
  - `./build/cxlmemsim_server --port 9999 --capacity 256`
  - Optional env: `SPDLOG_LEVEL=debug`, `CXL_BASE_ADDR=0`
- Quick demos/tests:
  - Shared-memory demo: `./run_demo.sh`
  - Protocol demo: `./run_protocol_demo.sh`
  - Direct test binaries: `./test_cxl_numa`, `./test_protocol_pattern`, etc.

## Coding Style & Naming
- Formatting: `.clang-format` (LLVM base, 4 spaces, 120 col). Run `clang-format -i` before committing.
- Linting: `.clang-tidy` enabled; prefer modern C++ idioms (`std::unique_ptr`, `noexcept`, `nullptr`).
- Naming: types `CamelCase`, functions `lowerCamelCase`, variables `snake_case`, constants `kCamelCase`.
- Headers in `include/`; keep public APIs minimal and documented in headers.

## Testing Guidelines
- Prefer small, self-contained executables in `src/` or `microbench/` for behavior checks.
- Add new demos or tests mirroring existing `test_*.c/cc` patterns; name clearly by feature (e.g., `test_numa_pattern.c`).
- Validate via scripts: `run_demo.sh`, `run_protocol_demo.sh`, `show_test_results.sh`.

## Commit & Pull Requests
- Commits: concise, component-scoped subject. Example: `server: add back-invalidation tracking`.
- Include rationale and any perf/latency impact in the body.
- PRs: describe change, link issues, include run commands and expected output, attach logs (e.g., `server_demo.log`) or screenshots.
- CI-style checks: build cleanly with Release and Debug, run both demos, pass `clang-format`/`clang-tidy`.

## Security & Configuration Tips
- Server mediates access; clients should use protocol paths (see `protocol_{reader,writer}.c`).
- Shared memory size and base are configured by server; avoid hardcoding `/dev/shm` offsets.
- Log levels via `SPDLOG_LEVEL` (e.g., `info`, `debug`) for reproducible reports.

