/*
 * Tests for the WASM-heap-backed SharedMemoryManager constructor.
 * Compiles natively (regression) and under emscripten via the same
 * source.
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 */
#include "shared_memory_manager.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>

int main() {
    SharedMemoryManager mgr(SharedMemoryManager::WasmHeapTag{}, /*capacity_mb=*/4);
    if (!mgr.initialize()) {
        std::fprintf(stderr, "FAIL: initialize() returned false\n");
        return 1;
    }

    uint8_t pattern[64];
    for (int i = 0; i < 64; ++i) pattern[i] = static_cast<uint8_t>(i);

    if (!mgr.write_cacheline(0x1000, pattern, 64)) {
        std::fprintf(stderr, "FAIL: write_cacheline\n");
        return 1;
    }
    uint8_t out[64] = {0};
    if (!mgr.read_cacheline(0x1000, out, 64)) {
        std::fprintf(stderr, "FAIL: read_cacheline\n");
        return 1;
    }
    if (std::memcmp(out, pattern, 64) != 0) {
        std::fprintf(stderr, "FAIL: data mismatch\n");
        return 1;
    }

    auto stats = mgr.get_stats();
    if (stats.total_capacity != 4ULL * 1024 * 1024) {
        std::fprintf(stderr, "FAIL: total_capacity = %zu\n", stats.total_capacity);
        return 1;
    }

    std::printf("OK\n");
    return 0;
}
