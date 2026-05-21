#include "cxlendpoint.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <tuple>
#include <vector>

#define REQUIRE(condition)                                                                                             \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::cerr << "Requirement failed: " #condition << "\n";                                                    \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

namespace {

BandwidthModelConfig test_model() {
    BandwidthModelConfig model;
    model.read_peak_gbps = 1.0;
    model.write_peak_gbps = 1.0;
    model.mixed_peak_gbps = 1.0;
    model.knee_utilization = 0.70;
    model.saturation_utilization = 0.90;
    model.min_window_ns = 1000;
    model.max_penalty_ns = 10000.0;
    model.calibrated_from_mlc = true;
    return model;
}

void add_local_range(CXLMemExpander &endpoint, uint64_t addr) {
    endpoint.occupation.push_back({0, addr, 0});
    endpoint.invalidate_cache();
}

std::vector<std::tuple<uint64_t, uint64_t>> repeated_accesses(uint64_t addr, size_t count, uint64_t spacing_ns) {
    std::vector<std::tuple<uint64_t, uint64_t>> accesses;
    accesses.reserve(count);
    for (size_t i = 0; i < count; i++) {
        accesses.emplace_back(static_cast<uint64_t>(i) * spacing_ns, addr);
    }
    return accesses;
}

} // namespace

int main() {
    CXLMemExpander endpoint(1, 1, 100, 120, 0, 64);
    endpoint.configure_bandwidth_model(test_model());
    add_local_range(endpoint, 0x1000);

    const auto low_pressure = repeated_accesses(0x1000, 4, 250000);
    const auto high_pressure = repeated_accesses(0x1000, 200, 1);

    const double low_penalty = endpoint.calculate_bandwidth(low_pressure);
    const double high_penalty = endpoint.calculate_bandwidth(high_pressure);
    REQUIRE(low_penalty >= 0.0);
    REQUIRE(high_penalty > low_penalty);

    CXLMemExpander sibling_endpoint(1, 1, 100, 120, 1, 64);
    sibling_endpoint.configure_bandwidth_model(test_model());
    add_local_range(sibling_endpoint, 0x8000);

    CXLSwitch child_a(10);
    CXLSwitch child_b(11);
    child_a.configure_bandwidth_model(test_model());
    child_b.configure_bandwidth_model(test_model());
    child_a.expanders.push_back(&endpoint);
    child_b.expanders.push_back(&sibling_endpoint);

    auto local_child_accesses = repeated_accesses(0x1000, 64, 1);
    auto mixed_child_accesses = local_child_accesses;
    auto sibling_accesses = repeated_accesses(0x8000, 256, 1);
    mixed_child_accesses.insert(mixed_child_accesses.end(), sibling_accesses.begin(), sibling_accesses.end());

    const double local_child_penalty = child_a.calculate_bandwidth(local_child_accesses);
    const double mixed_child_penalty = child_a.calculate_bandwidth(mixed_child_accesses);
    REQUIRE(std::fabs(local_child_penalty - mixed_child_penalty) < 0.000001);

    CXLSwitch root(20);
    root.configure_bandwidth_model(test_model());
    root.switches.push_back(&child_a);
    root.switches.push_back(&child_b);
    const double root_penalty = root.calculate_bandwidth(mixed_child_accesses);
    REQUIRE(root_penalty > local_child_penalty);

    std::cout << "Bandwidth model low=" << low_penalty << "ns high=" << high_penalty << "ns root=" << root_penalty
              << "ns\n";
    return 0;
}
