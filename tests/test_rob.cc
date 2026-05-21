#include "cxlendpoint.h"
#include "policy.h"
#include "rob.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
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

struct O3CpuTraceEvent {
    uint64_t tick = 0;
    uint64_t address = 0;
    bool memory = false;
    bool stall = false;
    std::string instruction;
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

std::optional<O3CpuTraceEvent> parse_o3cpu_debug_line(const std::string &line) {
    auto colon = line.find(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }

    O3CpuTraceEvent event;
    event.tick = std::stoull(line.substr(0, colon));
    std::string lowered = lower_copy(line);
    event.memory = lowered.find("load") != std::string::npos || lowered.find("store") != std::string::npos ||
                   lowered.find("lsq") != std::string::npos || lowered.find("mem") != std::string::npos;
    event.stall = lowered.find("stall") != std::string::npos;
    event.address = parse_hex_after(line, "addr=");

    auto inst_pos = line.find("inst=");
    event.instruction =
        inst_pos == std::string::npos ? (event.memory ? "ld r1, [mem]" : "add r1, r2") : line.substr(inst_pos + 5);
    return event;
}

InstructionGroup instruction_from_o3_event(const O3CpuTraceEvent &event) {
    InstructionGroup group;
    group.address = event.memory ? static_cast<long long>(event.address) : 0;
    group.fetchTimestamp = static_cast<long long>(event.tick);
    group.cycleCount = static_cast<long long>(event.tick);
    group.retireTimestamp = static_cast<long long>(event.tick + 1);
    group.instruction = event.instruction;
    return group;
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

void drain_rob(Rob &rob) {
    int ticks = 0;
    while (!rob.queue_.empty() && ticks < 4096) {
        rob.tick();
        ticks++;
    }
    REQUIRE(rob.queue_.empty());
}

} // namespace

int main() {
    controller = make_controller();
    Rob rob(controller, 16);

    std::vector<std::string> o3_debug = {
        "1000: system.cpu: O3CPU: T0 Fetch pc=0x400100 inst=add r1, r2",
        "1010: system.cpu: O3CPU: T0 IEW executing load addr=0x1000 inst=ld r3, [r4]",
        "1050: system.cpu: LSQUnit: T0 memory dependence stall addr=0x1000",
        "1150: system.cpu: O3CPU: T0 Commit load completed addr=0x1000",
    };

    std::vector<InstructionGroup> instructions;
    int stall_lines = 0;
    for (const auto &line : o3_debug) {
        auto event = parse_o3cpu_debug_line(line);
        REQUIRE(event.has_value());
        if (event->stall) {
            stall_lines++;
            continue;
        }
        if (line.find("Fetch") != std::string::npos || line.find("executing") != std::string::npos) {
            instructions.push_back(instruction_from_o3_event(*event));
        }
    }

    REQUIRE(stall_lines == 1);
    REQUIRE(instructions.size() == 2);
    REQUIRE(instructions[0].address == 0);
    REQUIRE(instructions[1].address == 0x1000);

    for (const auto &instruction : instructions) {
        REQUIRE(rob.issue(instruction));
        rob.tick();
    }
    drain_rob(rob);

    REQUIRE(rob.getCurrentCycle() > 1000);
    std::cout << "Parsed O3CPU stall lines: " << stall_lines << "\n";
    std::cout << "ROB stalls from O3CPU-derived instructions: " << rob.getStallCount() << "\n";
    return 0;
}
