/*
 * Near-switch runtime model for CXLMemSim.
 *
 * Models a small set of general-purpose and AI-oriented cores colocated with
 * the CXL switch/memory server. The model is synchronous: requests complete in
 * the server thread, while this class accounts for queueing and service time.
 */

#ifndef SWITCH_RUNTIME_H
#define SWITCH_RUNTIME_H

#include <cstdint>
#include <mutex>
#include <vector>

enum class SwitchCoreKind {
    General,
    AI,
    HardwareJIT,
};

struct SwitchHardwareJitWork {
    uint64_t bytes = 0;
    uint64_t output_bytes = 0;
    uint64_t work_items = 0;
    uint64_t emitted_commands = 1;
    uint32_t transform_mask = 0;
    uint32_t switchlet_id = 0;
    uint32_t ttl = 1;
    uint32_t max_ops = 1;
};

struct SwitchRuntimeConfig {
    bool enabled = false;
    uint32_t general_cores = 1;
    uint32_t ai_cores = 1;
    uint32_t hw_jit_lanes = 1;
    uint64_t general_base_latency_ns = 80;
    uint64_t ai_base_latency_ns = 200;
    uint64_t hw_jit_base_latency_ns = 12;
    uint64_t hw_jit_state_latency_ns = 8;
    double general_bandwidth_gbps = 64.0;
    double ai_ops_per_ns = 64.0;
    double hw_jit_bandwidth_gbps = 256.0;
    double hw_jit_ops_per_ns = 512.0;
};

struct SwitchRuntimeStats {
    uint64_t general_ops = 0;
    uint64_t ai_ops = 0;
    uint64_t general_bytes = 0;
    uint64_t ai_bytes = 0;
    uint64_t general_work_items = 0;
    uint64_t ai_work_items = 0;
    uint64_t hw_jit_ops = 0;
    uint64_t hw_jit_commands = 0;
    uint64_t hw_jit_bytes = 0;
    uint64_t hw_jit_output_bytes = 0;
    uint64_t hw_jit_work_items = 0;
    uint64_t hw_jit_service_ns = 0;
    uint64_t queued_ns = 0;
    uint64_t service_ns = 0;
};

class SwitchRuntime {
public:
    explicit SwitchRuntime(const SwitchRuntimeConfig &config = {});

    bool enabled() const { return config_.enabled; }
    const SwitchRuntimeConfig &config() const { return config_; }

    uint64_t dispatch(SwitchCoreKind kind, uint64_t bytes, uint64_t work_items, uint64_t request_time_ns);
    uint64_t dispatch_hardware_jit(const SwitchHardwareJitWork &work, uint64_t request_time_ns);
    SwitchRuntimeStats get_stats() const;

private:
    uint64_t compute_service_ns(SwitchCoreKind kind, uint64_t bytes, uint64_t work_items) const;
    uint64_t compute_hardware_jit_service_ns(const SwitchHardwareJitWork &work) const;

    SwitchRuntimeConfig config_;
    std::vector<uint64_t> general_ready_ns_;
    std::vector<uint64_t> ai_ready_ns_;
    std::vector<uint64_t> hw_jit_ready_ns_;
    SwitchRuntimeStats stats_;
    mutable std::mutex mutex_;
};

const char *switch_core_kind_name(SwitchCoreKind kind);

#endif // SWITCH_RUNTIME_H
