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

#ifndef __EMSCRIPTEN__
#include <sys/mman.h>
#endif

namespace {
uint32_t to_offset(void *p) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
}

/* In native 64-bit builds the regular heap is above 4 GB, so we
   cannot pass raw pointers through a uint32_t ABI.  Allocate a small
   arena via mmap(MAP_32BIT) which is guaranteed to land below 2 GB. */
#ifndef __EMSCRIPTEN__
struct Arena32 {
    void *base;
    size_t size;
    size_t used;

    explicit Arena32(size_t sz) : size(sz), used(0) {
        base = mmap(nullptr, sz, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (base == MAP_FAILED) base = nullptr;
    }
    ~Arena32() {
        if (base) munmap(base, size);
    }
    uint8_t *alloc(size_t n, size_t align = 16) {
        uintptr_t p = (reinterpret_cast<uintptr_t>(base) + used + align - 1)
                      & ~(align - 1);
        used = (p - reinterpret_cast<uintptr_t>(base)) + n;
        return reinterpret_cast<uint8_t *>(p);
    }
};
#endif
}

int main() {
    uint32_t pool_base = 0xFFFFFFFFu;
    if (cxlmemsim_init(4 * 1024 * 1024, "", &pool_base) != 0) {
        std::fprintf(stderr, "FAIL: cxlmemsim_init\n");
        return 1;
    }

    /* ServerRequest layout: see CXLMemSim/src/main_server.cc.
       op_type(1) addr(8) size(8) timestamp(8) value(8) expected(8) data[64] */
    constexpr int REQ_SIZE = 1 + 8 + 8 + 8 + 8 + 8 + 64; /* 105 */
    constexpr int RESP_SIZE = 1 + 8 + 8 + 64;            /* 81  */
    std::vector<uint8_t> req(REQ_SIZE, 0);
    std::vector<uint8_t> resp(RESP_SIZE, 0);

#ifndef __EMSCRIPTEN__
    /* In native 64-bit builds, allocate buffers in the low 2 GB so
       that the uint32_t ABI round-trip is lossless. */
    constexpr size_t ARENA_SIZE = 4096;
    Arena32 arena(ARENA_SIZE);
    if (!arena.base) {
        std::fprintf(stderr, "FAIL: arena32 mmap\n");
        return 1;
    }
    uint8_t *req_buf  = arena.alloc(REQ_SIZE);
    uint8_t *resp_buf = arena.alloc(RESP_SIZE);
    uint32_t *inv_buf = reinterpret_cast<uint32_t *>(arena.alloc(16 * sizeof(uint32_t)));
    std::memset(req_buf, 0, REQ_SIZE);
    std::memset(resp_buf, 0, RESP_SIZE);
    std::memset(inv_buf, 0, 16 * sizeof(uint32_t));

    auto write_u64 = [&](size_t off, uint64_t v) {
        std::memcpy(req_buf + off, &v, sizeof(v));
    };

    /* WRITE: op=1, addr=0x1000, size=64, data=pattern */
    req_buf[0] = 1; write_u64(1, 0x1000); write_u64(9, 64);
    write_u64(17, 0); write_u64(25, 0); write_u64(33, 0);
    for (int i = 0; i < 64; ++i) req_buf[41 + i] = static_cast<uint8_t>(i ^ 0xA5);

    int32_t n = cxlmemsim_handle_request(to_offset(req_buf),
                                          to_offset(resp_buf),
                                          to_offset(inv_buf), 16);
    if (n < 0 || resp_buf[0] != 0) {
        std::fprintf(stderr, "FAIL: write request status=%d n=%d\n",
                     resp_buf[0], n);
        return 1;
    }

    /* READ: op=0, addr=0x1000, size=64 */
    std::memset(req_buf, 0, REQ_SIZE);
    std::memset(resp_buf, 0, RESP_SIZE);
    req_buf[0] = 0; write_u64(1, 0x1000); write_u64(9, 64);

    n = cxlmemsim_handle_request(to_offset(req_buf),
                                  to_offset(resp_buf),
                                  to_offset(inv_buf), 16);
    if (n < 0 || resp_buf[0] != 0) {
        std::fprintf(stderr, "FAIL: read request status=%d n=%d\n",
                     resp_buf[0], n);
        return 1;
    }
    for (int i = 0; i < 64; ++i) {
        if (resp_buf[17 + i] != static_cast<uint8_t>(i ^ 0xA5)) {
            std::fprintf(stderr,
                "FAIL: read mismatch at %d (got %u expected %u)\n",
                i, resp_buf[17 + i], (i ^ 0xA5));
            return 1;
        }
    }
#else
    /* WASM/Emscripten path: heap is 32-bit linear, vectors work fine. */
    auto write_u64 = [&](size_t off, uint64_t v) {
        std::memcpy(req.data() + off, &v, sizeof(v));
    };

    /* WRITE: op=1, addr=0x1000, size=64, data=pattern */
    req[0] = 1; write_u64(1, 0x1000); write_u64(9, 64);
    write_u64(17, 0); write_u64(25, 0); write_u64(33, 0);
    for (int i = 0; i < 64; ++i) req[41 + i] = static_cast<uint8_t>(i ^ 0xA5);

    uint32_t inv[16] = {0};
    int32_t n = cxlmemsim_handle_request(to_offset(req.data()),
                                          to_offset(resp.data()),
                                          to_offset(inv), 16);
    if (n < 0 || resp[0] != 0) {
        std::fprintf(stderr, "FAIL: write request status=%d n=%d\n",
                     resp[0], n);
        return 1;
    }

    /* READ: op=0, addr=0x1000, size=64 */
    std::memset(req.data(), 0, REQ_SIZE);
    std::memset(resp.data(), 0, RESP_SIZE);
    req[0] = 0; write_u64(1, 0x1000); write_u64(9, 64);

    n = cxlmemsim_handle_request(to_offset(req.data()),
                                  to_offset(resp.data()),
                                  to_offset(inv), 16);
    if (n < 0 || resp[0] != 0) {
        std::fprintf(stderr, "FAIL: read request status=%d n=%d\n",
                     resp[0], n);
        return 1;
    }
    for (int i = 0; i < 64; ++i) {
        if (resp[17 + i] != static_cast<uint8_t>(i ^ 0xA5)) {
            std::fprintf(stderr,
                "FAIL: read mismatch at %d (got %u expected %u)\n",
                i, resp[17 + i], (i ^ 0xA5));
            return 1;
        }
    }
#endif

    cxlmemsim_reset();
    std::printf("OK\n");
    return 0;
}
