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

static uint64_t popcount_u32(uint32_t value) {
    uint64_t count = 0;
    while (value != 0) {
        count += value & 1u;
        value >>= 1u;
    }
    return count;
}

} // namespace

SwitchRuntime::SwitchRuntime(const SwitchRuntimeConfig &config) : config_(config) {
    if (config_.general_cores == 0) {
        config_.general_cores = 1;
    }
    if (config_.ai_cores == 0) {
        config_.ai_cores = 1;
    }
    if (config_.hw_jit_lanes == 0) {
        config_.hw_jit_lanes = 1;
    }
    if (config_.general_bandwidth_gbps <= 0.0) {
        config_.general_bandwidth_gbps = 64.0;
    }
    if (config_.ai_ops_per_ns <= 0.0) {
        config_.ai_ops_per_ns = 64.0;
    }
    if (config_.hw_jit_bandwidth_gbps <= 0.0) {
        config_.hw_jit_bandwidth_gbps = 256.0;
    }
    if (config_.hw_jit_ops_per_ns <= 0.0) {
        config_.hw_jit_ops_per_ns = 512.0;
    }

    general_ready_ns_.assign(config_.general_cores, 0);
    ai_ready_ns_.assign(config_.ai_cores, 0);
    hw_jit_ready_ns_.assign(config_.hw_jit_lanes, 0);
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
    case SwitchCoreKind::HardwareJIT:
        break;
    }
    return 0;
}

uint64_t SwitchRuntime::compute_hardware_jit_service_ns(const SwitchHardwareJitWork &work) const {
    uint64_t transfer_ns =
        ceil_to_u64(static_cast<double>(work.bytes + work.output_bytes) / config_.hw_jit_bandwidth_gbps);
    uint64_t compute_ns = ceil_to_u64(static_cast<double>(work.work_items) / config_.hw_jit_ops_per_ns);
    uint64_t transform_ns = 2 * popcount_u32(work.transform_mask);
    uint64_t command_ns = work.emitted_commands == 0 ? 0 : work.emitted_commands - 1;

    return config_.hw_jit_base_latency_ns + config_.hw_jit_state_latency_ns + transform_ns + command_ns +
           transfer_ns + compute_ns;
}

uint64_t SwitchRuntime::dispatch(SwitchCoreKind kind, uint64_t bytes, uint64_t work_items, uint64_t request_time_ns) {
    if (!config_.enabled) {
        return 0;
    }
    if (kind == SwitchCoreKind::HardwareJIT) {
        SwitchHardwareJitWork work;
        work.bytes = bytes;
        work.output_bytes = bytes;
        work.work_items = work_items;
        return dispatch_hardware_jit(work, request_time_ns);
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

uint64_t SwitchRuntime::dispatch_hardware_jit(const SwitchHardwareJitWork &work, uint64_t request_time_ns) {
    if (!config_.enabled) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto selected = std::min_element(hw_jit_ready_ns_.begin(), hw_jit_ready_ns_.end());
    uint64_t start_ns = std::max(request_time_ns, *selected);
    uint64_t queued_ns = start_ns - request_time_ns;
    uint64_t service_ns = compute_hardware_jit_service_ns(work);
    *selected = start_ns + service_ns;

    stats_.queued_ns += queued_ns;
    stats_.service_ns += service_ns;
    stats_.hw_jit_ops++;
    stats_.hw_jit_commands += work.emitted_commands;
    stats_.hw_jit_bytes += work.bytes;
    stats_.hw_jit_output_bytes += work.output_bytes;
    stats_.hw_jit_work_items += work.work_items;
    stats_.hw_jit_service_ns += service_ns;

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
    case SwitchCoreKind::HardwareJIT:
        return "hardware_jit";
    }
    return "unknown";
}
