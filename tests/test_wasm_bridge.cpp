/*
 * Tests for the wasm_bridge C ABI. Calls the exports as if from JS,
 * but linked natively so we can debug under gdb. The same source is
 * also compiled under emscripten and run by node from the build
 * script's smoke test.
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 */
#include "wasm_bridge.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
uint32_t to_offset(void *p) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}
}

int main() {
    uint32_t pool_base = 0xFFFFFFFFu;
    int rc = cxlmemsim_init(4 * 1024 * 1024, "", &pool_base);
    if (rc != 0) {
        std::fprintf(stderr, "FAIL: cxlmemsim_init rc=%d\n", rc);
        return 1;
    }
    /* pool_base is a WASM-heap offset; in the native build, it is
       the lower 32 bits of a host pointer — non-zero is enough. */
    if (pool_base == 0xFFFFFFFFu) {
        std::fprintf(stderr, "FAIL: out_pool_base not written\n");
        return 1;
    }

    cxlmemsim_reset();

    std::printf("OK\n");
    return 0;
}
