/*
 * Near-switch runtime model for CXLMemSim.
 */

#include "switch_runtime.h"

#include <algorithm>
#include <cmath>

namespace {

static uint64_t ceil_to_u64(double value) {
    if (value <= 0.0) {
        return 0;
    }
    return static_cast<uint64_t>(std::ceil(value));
}

} // namespace

SwitchRuntime::SwitchRuntime(const SwitchRuntimeConfig &config) : config_(config) {
    if (config_.general_cores == 0) {
        config_.general_cores = 1;
    }
    if (config_.ai_cores == 0) {
        config_.ai_cores = 1;
    }
    if (config_.general_bandwidth_gbps <= 0.0) {
        config_.general_bandwidth_gbps = 64.0;
    }
    if (config_.ai_ops_per_ns <= 0.0) {
        config_.ai_ops_per_ns = 64.0;
    }

    general_ready_ns_.assign(config_.general_cores, 0);
    ai_ready_ns_.assign(config_.ai_cores, 0);
}

uint64_t SwitchRuntime::compute_service_ns(SwitchCoreKind kind, uint64_t bytes, uint64_t work_items) const {
    switch (kind) {
    case SwitchCoreKind::General: {
        // 1 GB/s is approximately 1 byte/ns.
        uint64_t transfer_ns = ceil_to_u64(static_cast<double>(bytes) / config_.general_bandwidth_gbps);
        return config_.general_base_latency_ns + transfer_ns + work_items;
    }
    case SwitchCoreKind::AI: {
        uint64_t compute_ns = ceil_to_u64(static_cast<double>(work_items) / config_.ai_ops_per_ns);
        uint64_t transfer_ns = ceil_to_u64(static_cast<double>(bytes) / config_.general_bandwidth_gbps);
        return config_.ai_base_latency_ns + compute_ns + transfer_ns;
    }
    }
    return 0;
}

uint64_t SwitchRuntime::dispatch(SwitchCoreKind kind, uint64_t bytes, uint64_t work_items, uint64_t request_time_ns) {
    if (!config_.enabled) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<uint64_t> &ready = (kind == SwitchCoreKind::AI) ? ai_ready_ns_ : general_ready_ns_;
    auto selected = std::min_element(ready.begin(), ready.end());
    uint64_t start_ns = std::max(request_time_ns, *selected);
    uint64_t queued_ns = start_ns - request_time_ns;
    uint64_t service_ns = compute_service_ns(kind, bytes, work_items);
    *selected = start_ns + service_ns;

    stats_.queued_ns += queued_ns;
    stats_.service_ns += service_ns;
    if (kind == SwitchCoreKind::AI) {
        stats_.ai_ops++;
        stats_.ai_bytes += bytes;
        stats_.ai_work_items += work_items;
    } else {
        stats_.general_ops++;
        stats_.general_bytes += bytes;
        stats_.general_work_items += work_items;
    }

    return queued_ns + service_ns;
}

SwitchRuntimeStats SwitchRuntime::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

const char *switch_core_kind_name(SwitchCoreKind kind) {
    switch (kind) {
    case SwitchCoreKind::General:
        return "general";
    case SwitchCoreKind::AI:
        return "ai";
    }
    return "unknown";
}
