#include "cxlendpoint.h"
#include "policy.h"
#include "rob.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#define REQUIRE(condition)                                                                                             \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::cerr << "Requirement failed: " #condition << "\n";                                                    \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

Helper helper{};
CXLController *controller = nullptr;
Monitors *monitors = nullptr;

namespace {

struct O3StallSample {
    uint64_t tick = 0;
    uint64_t address = 0;
    bool stall_start = false;
    bool stall_end = false;
};

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

uint64_t parse_hex_after(const std::string &line, const std::string &key) {
    auto key_pos = line.find(key);
    if (key_pos == std::string::npos) {
        return 0;
    }
    auto value_pos = line.find("0x", key_pos + key.size());
    if (value_pos == std::string::npos) {
        return 0;
    }
    size_t consumed = 0;
    return std::stoull(line.substr(value_pos), &consumed, 16);
}

std::optional<O3StallSample> parse_o3_stall_line(const std::string &line) {
    auto colon = line.find(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }

    std::string lowered = lower_copy(line);
    O3StallSample sample;
    sample.tick = std::stoull(line.substr(0, colon));
    sample.address = parse_hex_after(line, "addr=");
    sample.stall_start = lowered.find("stall") != std::string::npos && lowered.find("release") == std::string::npos &&
                         lowered.find("complete") == std::string::npos;
    sample.stall_end = lowered.find("release") != std::string::npos || lowered.find("complete") != std::string::npos ||
                       lowered.find("response") != std::string::npos;
    return sample;
}

uint64_t o3_memory_stall_cycles(const std::vector<std::string> &lines, uint64_t address) {
    uint64_t start = 0;
    uint64_t total = 0;
    for (const auto &line : lines) {
        auto sample = parse_o3_stall_line(line);
        REQUIRE(sample.has_value());
        if (sample->address != address) {
            continue;
        }
        if (sample->stall_start && start == 0) {
            start = sample->tick;
        } else if (sample->stall_end && start != 0 && sample->tick > start) {
            total += sample->tick - start;
            start = 0;
        }
    }
    return total;
}

CXLController *make_controller() {
    std::array<Policy *, 4> policies = {
        new InterleavePolicy(),
        new MigrationPolicy(),
        new PagingPolicy(),
        new CachingPolicy(),
    };
    auto *ctrl = new CXLController(policies, 64, CACHELINE, 100, 110);
    ctrl->insert_end_point(new CXLMemExpander(50, 50, 100, 140, 0, 64));
    ctrl->construct_topo("(1);");
    return ctrl;
}

int retire_instruction_and_count_cycles(Rob &rob, const InstructionGroup &instruction) {
    REQUIRE(rob.issue(instruction));
    int ticks = 0;
    while (!rob.queue_.empty() && ticks < 4096) {
        rob.tick();
        ticks++;
    }
    REQUIRE(rob.queue_.empty());
    return ticks;
}

} // namespace

int main() {
    constexpr uint64_t kLoadAddr = 0x2000;
    std::vector<std::string> o3_debug = {
        "2000: system.cpu: O3CPU: T0 IEW execute load addr=0x2000 inst=ld r1, [r2]",
        "2020: system.cpu: LSQUnit: T0 memory stall waiting for cache fill addr=0x2000",
        "2180: system.cpu: O3CPU: T0 memory response complete addr=0x2000",
        "2200: system.cpu: O3CPU: T0 Commit load addr=0x2000",
    };

    uint64_t o3_stall_cycles = o3_memory_stall_cycles(o3_debug, kLoadAddr);
    REQUIRE(o3_stall_cycles == 160);

    controller = make_controller();
    Rob rob(controller, 16);

    InstructionGroup mem_ins;
    mem_ins.instruction = "ld r1, [r2]";
    mem_ins.address = kLoadAddr;
    mem_ins.fetchTimestamp = 2000;
    mem_ins.cycleCount = 2000;
    mem_ins.retireTimestamp = 2001;

    int mem_ticks = retire_instruction_and_count_cycles(rob, mem_ins);
    int64_t modeled_mem_stalls = rob.getStallCount();
    REQUIRE(mem_ticks > 0);
    REQUIRE(modeled_mem_stalls > 0);

    rob.resetCounters();
    InstructionGroup nonmem_ins;
    nonmem_ins.instruction = "add r1, r2";
    nonmem_ins.address = 0;
    nonmem_ins.fetchTimestamp = 2300;
    nonmem_ins.cycleCount = 2300;
    nonmem_ins.retireTimestamp = 2301;

    int nonmem_ticks = retire_instruction_and_count_cycles(rob, nonmem_ins);
    int64_t modeled_nonmem_stalls = rob.getStallCount();

    REQUIRE(mem_ticks >= nonmem_ticks);
    REQUIRE(modeled_mem_stalls > modeled_nonmem_stalls);

    double ratio = static_cast<double>(modeled_mem_stalls) / static_cast<double>(o3_stall_cycles);
    REQUIRE(ratio > 0.0);
    REQUIRE(ratio < 20.0);

    std::cout << "O3CPU memory stall cycles: " << o3_stall_cycles << "\n";
    std::cout << "Modeled memory stalls: " << modeled_mem_stalls << "\n";
    std::cout << "Modeled non-memory stalls: " << modeled_nonmem_stalls << "\n";
    return 0;
}
