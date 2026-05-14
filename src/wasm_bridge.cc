#include "wasm_bridge.h"

#include "cxlcontroller.h"
#include "cxlendpoint.h"
#include "coherency_engine.h"
#include "policy.h"
#include "shared_memory_manager.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#define KEEPALIVE
#endif

namespace {

struct BridgeState {
    std::unique_ptr<SharedMemoryManager> shm;
    std::unique_ptr<InterleavePolicy> alloc_policy;
    std::unique_ptr<HeatAwareMigrationPolicy> migration_policy;
    std::unique_ptr<HugePagePolicy> paging_policy;
    std::unique_ptr<FIFOPolicy> caching_policy;
    std::unique_ptr<CXLMemExpander> endpoint;
    std::unique_ptr<CXLController> controller;

    std::atomic<uint64_t> total_reads{0};
    std::atomic<uint64_t> total_writes{0};
    std::atomic<uint64_t> total_atomics{0};
    std::atomic<uint64_t> total_invalidations{0};
    std::atomic<uint64_t> total_latency_ns{0};
    std::atomic<uint64_t> total_errors{0};
};

BridgeState *g_state = nullptr;

void teardown() {
    delete g_state;
    g_state = nullptr;
}

bool build_default_topology(BridgeState &s, std::string_view topo) {
    s.alloc_policy = std::make_unique<InterleavePolicy>();
    s.migration_policy = std::make_unique<HeatAwareMigrationPolicy>();
    s.paging_policy = std::make_unique<HugePagePolicy>();
    s.caching_policy = std::make_unique<FIFOPolicy>();

    std::array<Policy *, 4> policies{
        s.alloc_policy.get(),
        s.migration_policy.get(),
        s.paging_policy.get(),
        s.caching_policy.get()
    };

    int capacity_mb = static_cast<int>(s.shm->get_stats().total_capacity /
                                       (1024ULL * 1024ULL));
    s.controller = std::make_unique<CXLController>(
        policies, capacity_mb, PAGE, /*epoch=*/1, /*dramlatency=*/85.0);

    s.endpoint = std::make_unique<CXLMemExpander>(
        /*read_bw=*/40000, /*write_bw=*/40000,
        /*read_lat=*/180, /*write_lat=*/200,
        /*id=*/0, /*capacity_mb=*/capacity_mb);
    s.controller->insert_end_point(s.endpoint.get());

    const std::string newick = topo.empty() ? "(1);" : std::string(topo);
    try {
        s.controller->construct_topo(newick);
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

} /* namespace */

extern "C" {

KEEPALIVE int cxlmemsim_init(uint32_t pool_capacity_bytes,
                             const char *topology_json,
                             uint32_t *out_pool_base) {
    teardown();
    auto state = std::make_unique<BridgeState>();

    size_t capacity_mb = pool_capacity_bytes / (1024ULL * 1024ULL);
    if (capacity_mb == 0) capacity_mb = 4;

    state->shm = std::make_unique<SharedMemoryManager>(
        SharedMemoryManager::WasmHeapTag{}, capacity_mb);
    if (!state->shm->initialize()) {
        return 1;
    }
    if (!build_default_topology(*state, topology_json ? topology_json : "")) {
        return 2;
    }

    if (out_pool_base) {
        uint8_t *base = static_cast<uint8_t *>(state->shm->get_data_area());
        *out_pool_base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base));
    }
    g_state = state.release();
    return 0;
}

KEEPALIVE int32_t cxlmemsim_handle_request(uint32_t req_ptr,
                                           uint32_t resp_ptr,
                                           uint32_t inv_out_ptr,
                                           uint32_t inv_cap) {
    if (!g_state) return -1;

    const uint8_t *req = reinterpret_cast<const uint8_t *>(
        static_cast<uintptr_t>(req_ptr));
    uint8_t *resp = reinterpret_cast<uint8_t *>(
        static_cast<uintptr_t>(resp_ptr));

    auto load_u64 = [](const uint8_t *p) {
        uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    };
    auto store_u64 = [](uint8_t *p, uint64_t v) {
        std::memcpy(p, &v, sizeof(v));
    };

    uint8_t op = req[0];
    uint64_t addr = load_u64(req + 1);
    uint64_t size = load_u64(req + 9);
    uint64_t value = load_u64(req + 25);
    uint64_t expected = load_u64(req + 33);

    std::memset(resp, 0, 81);

    constexpr uint8_t OP_READ = 0;
    constexpr uint8_t OP_WRITE = 1;
    constexpr uint8_t OP_ATOMIC_FAA = 3;
    constexpr uint8_t OP_ATOMIC_CAS = 4;
    constexpr uint8_t OP_FENCE = 5;
    constexpr uint8_t OP_LSA_READ = 6;
    constexpr uint8_t OP_LSA_WRITE = 7;

    /* Clamp size to one cacheline payload. */
    if (size > 64) size = 64;

    uint64_t old_value = 0;
    uint64_t latency_ns = static_cast<uint64_t>(g_state->controller->dramlatency);
    uint8_t status = 0;
    int32_t invalidations = 0;

    switch (op) {
    case OP_READ:
    case OP_LSA_READ: {
        if (!g_state->shm->read_cacheline(addr, resp + 17, size)) {
            status = 2;
            break;
        }
        g_state->total_reads++;
        break;
    }
    case OP_WRITE:
    case OP_LSA_WRITE: {
        if (!g_state->shm->write_cacheline(addr, req + 41, size)) {
            status = 2;
            break;
        }
        g_state->total_writes++;
        if (inv_out_ptr && inv_cap > 0) {
            uint32_t *inv = reinterpret_cast<uint32_t *>(
                static_cast<uintptr_t>(inv_out_ptr));
            inv[0] = static_cast<uint32_t>(addr & ~uint64_t{63});
            invalidations = 1;
            g_state->total_invalidations++;
        }
        break;
    }
    case OP_ATOMIC_FAA: {
        uint8_t buf[8] = {0};
        if (!g_state->shm->read_cacheline(addr, buf, 8)) { status = 2; break; }
        std::memcpy(&old_value, buf, 8);
        uint64_t newv = old_value + value;
        std::memcpy(buf, &newv, 8);
        if (!g_state->shm->write_cacheline(addr, buf, 8)) { status = 2; break; }
        g_state->total_atomics++;
        break;
    }
    case OP_ATOMIC_CAS: {
        uint8_t buf[8] = {0};
        if (!g_state->shm->read_cacheline(addr, buf, 8)) { status = 2; break; }
        std::memcpy(&old_value, buf, 8);
        if (old_value == expected) {
            std::memcpy(buf, &value, 8);
            if (!g_state->shm->write_cacheline(addr, buf, 8)) { status = 2; break; }
        }
        g_state->total_atomics++;
        break;
    }
    case OP_FENCE:
        break;
    default:
        status = 3;
        break;
    }

    if (status != 0) {
        g_state->total_errors++;
    } else {
        g_state->total_latency_ns += latency_ns;
    }
    resp[0] = status;
    store_u64(resp + 1, latency_ns);
    store_u64(resp + 9, old_value);
    return invalidations;
}

KEEPALIVE void cxlmemsim_handle_type2(uint32_t msg_ptr) {
    if (!g_state) return;
    const uint8_t *msg = reinterpret_cast<const uint8_t *>(
        static_cast<uintptr_t>(msg_ptr));

    /* Type-2 message header layout (see cxlmemsim-pool-worker.js
       makeType2Message): type(4) size(4) addr(8) timestamp(8)
       state(1) source(1) reserved(0..) data[26..90] */
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    std::memcpy(&type, msg + 0, 4);
    std::memcpy(&size, msg + 4, 4);
    std::memcpy(&addr, msg + 8, 8);
    if (size > 64) size = 64;

    constexpr uint32_t T2_WRITE = 2;
    constexpr uint32_t T2_WRITEBACK = 8;
    constexpr uint32_t T2_GPU_ACCESS = 9;
    constexpr uint32_t T2_INVALIDATE = 7;
    constexpr uint32_t T2_FLUSH = 3;

    if (type == T2_WRITE || type == T2_WRITEBACK || type == T2_GPU_ACCESS) {
        if (g_state->shm->write_cacheline(addr, msg + 26, size)) {
            g_state->total_writes++;
            g_state->total_invalidations++;
        }
    } else if (type == T2_INVALIDATE || type == T2_FLUSH) {
        g_state->total_invalidations++;
    }
}

KEEPALIVE void cxlmemsim_get_stats(uint32_t out_ptr) {
    cxlmemsim_stats_t s{};
    if (g_state) {
        s.total_reads = g_state->total_reads.load();
        s.total_writes = g_state->total_writes.load();
        s.total_atomics = g_state->total_atomics.load();
        s.total_invalidations = g_state->total_invalidations.load();
        s.total_errors = g_state->total_errors.load();
        s.total_latency_ns = g_state->total_latency_ns.load();
        s.pool_capacity_bytes = g_state->shm
            ? g_state->shm->get_stats().total_capacity
            : 0;
        /* MESI histogram (s.mesi_invalid / _shared / _exclusive /
           _modified) is left at zero — coherency-engine wiring is a
           future task. bytes_read / bytes_written are also zero for
           now; the bridge counts ops, not bytes — JS-side stats cover
           the byte totals. */
    }
    std::memcpy(reinterpret_cast<void *>(static_cast<uintptr_t>(out_ptr)),
                &s, sizeof(s));
}

KEEPALIVE void cxlmemsim_reset(void) {
    if (!g_state) return;
    teardown();
}

} /* extern "C" */
