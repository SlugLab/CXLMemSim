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
    std::unique_ptr<CXLController> controller;
    std::unique_ptr<InterleavePolicy> alloc_policy;
    std::unique_ptr<HeatAwareMigrationPolicy> migration_policy;
    std::unique_ptr<HugePagePolicy> paging_policy;
    std::unique_ptr<FIFOPolicy> caching_policy;
    std::unique_ptr<CXLMemExpander> endpoint;

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
    s.controller->construct_topo(newick);
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

KEEPALIVE int32_t cxlmemsim_handle_request(uint32_t /*req_ptr*/,
                                           uint32_t /*resp_ptr*/,
                                           uint32_t /*inv_out_ptr*/,
                                           uint32_t /*inv_cap*/) {
    return -1; /* implemented in Task 4 */
}

KEEPALIVE void cxlmemsim_handle_type2(uint32_t /*msg_ptr*/) {}

KEEPALIVE void cxlmemsim_get_stats(uint32_t /*out_ptr*/) {}

KEEPALIVE void cxlmemsim_reset(void) {
    if (!g_state) return;
    g_state->shm->cleanup();
    g_state->shm.reset();
    teardown();
}

} /* extern "C" */
