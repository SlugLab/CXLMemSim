#include "switch_runtime.h"

#include <cstdint>
#include <iostream>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) {
        return true;
    }

    std::cerr << "switch runtime scheduler test failed: " << message << '\n';
    return false;
}

} // namespace

int main() {
    SwitchRuntimeConfig config;
    config.enabled = true;
    config.general_cores = 1;
    config.ai_cores = 2;
    config.hw_jit_lanes = 1;
    config.general_base_latency_ns = 10;
    config.ai_base_latency_ns = 20;
    config.hw_jit_base_latency_ns = 8;
    config.hw_jit_state_latency_ns = 4;
    config.general_bandwidth_gbps = 16.0;
    config.ai_ops_per_ns = 4.0;
    config.hw_jit_bandwidth_gbps = 128.0;
    config.hw_jit_ops_per_ns = 32.0;

    SwitchRuntime runtime(config);

    uint64_t general_a = runtime.dispatch(SwitchCoreKind::General, 64, 1, 100);
    uint64_t general_b = runtime.dispatch(SwitchCoreKind::General, 64, 1, 100);
    uint64_t ai_a = runtime.dispatch(SwitchCoreKind::AI, 128, 16, 100);
    uint64_t ai_b = runtime.dispatch(SwitchCoreKind::AI, 128, 16, 100);
    uint64_t ai_c = runtime.dispatch(SwitchCoreKind::AI, 128, 16, 100);
    SwitchHardwareJitWork hwjit_work;
    hwjit_work.bytes = 1024;
    hwjit_work.output_bytes = 512;
    hwjit_work.work_items = 256;
    hwjit_work.emitted_commands = 1;
    hwjit_work.transform_mask = 1;
    hwjit_work.switchlet_id = 1;
    hwjit_work.ttl = 4;
    hwjit_work.max_ops = 1;
    uint64_t hwjit_a = runtime.dispatch_hardware_jit(hwjit_work, 100);
    uint64_t hwjit_b = runtime.dispatch_hardware_jit(hwjit_work, 100);

    bool passed = true;
    passed &= expect(general_a == 15, "first general op should have expected service latency");
    passed &= expect(general_b > general_a, "second general op should queue behind the first");
    passed &= expect(ai_a == 32, "first AI op should have expected service latency");
    passed &= expect(ai_b == 32, "second AI op should run on the second AI core");
    passed &= expect(ai_c > ai_a, "third AI op should queue behind a busy AI core");
    passed &= expect(hwjit_a == 34, "first Hardware-JIT op should have expected service latency");
    passed &= expect(hwjit_b > hwjit_a, "second Hardware-JIT op should queue behind the first");

    SwitchRuntimeStats stats = runtime.get_stats();
    passed &= expect(stats.general_ops == 2, "general op count should match dispatch count");
    passed &= expect(stats.ai_ops == 3, "AI op count should match dispatch count");
    passed &= expect(stats.hw_jit_ops == 2, "Hardware-JIT op count should match dispatch count");
    passed &= expect(stats.hw_jit_commands == 2, "Hardware-JIT command count should match emitted effects");
    passed &= expect(stats.general_bytes == 128, "general byte count should match dispatch bytes");
    passed &= expect(stats.ai_bytes == 384, "AI byte count should match dispatch bytes");
    passed &= expect(stats.hw_jit_bytes == 2048, "Hardware-JIT byte count should match dispatch bytes");
    passed &= expect(stats.hw_jit_output_bytes == 1024, "Hardware-JIT output byte count should match dispatch bytes");
    passed &= expect(stats.ai_work_items == 48, "AI work item count should match dispatch work");
    passed &= expect(stats.hw_jit_work_items == 512, "Hardware-JIT work item count should match dispatch work");
    passed &= expect(stats.hw_jit_service_ns == 68, "Hardware-JIT service time should be accumulated");
    passed &= expect(stats.service_ns > 0, "service time should be accumulated");
    passed &= expect(stats.queued_ns > 0, "queued time should be accumulated");

    if (!passed) {
        return 1;
    }

    std::cout << "switch runtime scheduler test passed\n";
    return 0;
}
