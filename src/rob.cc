#include "rob.h"
#include "policy.h"
#include <cxxopts.hpp>
#include <fstream>
#include <iostream>
#include <ranges>
#include <spdlog/cfg/env.h>
#include <sstream>

// 发射指令到ROB
bool Rob::issue(const InstructionGroup &ins) {
    if (queue_.size() >= maxSize_) {
        stallCount_++;
        return false; // ROB已满,停顿
    }

    // 将指令加入ROB尾部
    queue_.push_back(ins);

    // 对于内存访问指令,通知控制器
    if (ins.address != 0) {
        controller_->insert(ins.retireTimestamp, 0, ins.address, 0);
    }

    return true;
}

// 检查指令是否可以提交
bool Rob::canRetire(const InstructionGroup &ins) {
    if (ins.address == 0) {
        return true; // 非内存指令可以直接提交
    }

    // 检查内存访问是否完成
    if (cur_latency == 0) {
        auto allAccess = controller_->get_all_access();
        cur_latency = controller_->calculate_latency(LatencyPass{allAccess, 80, 1, 1});
    }
    // SPDLOG_INFO("{}",cur_latency);
    if (currentCycle_ - ins.cycleCount >= cur_latency) {
        cur_latency = 0;
        return true;
    }
    return false;
}

// 提交最老的指令
void Rob::retire() {
    if (queue_.empty()) {
        return;
    }

    auto &oldestIns = queue_.front();
    if (!canRetire(oldestIns)) {
        stallCount_++; // 无法提交,增加停顿
        return;
    }

    // 计算这条指令的实际延迟
    if (oldestIns.address != 0) {
        auto allAccess = controller_->get_all_access();
        uint64_t latency = controller_->calculate_latency(LatencyPass{allAccess, 80, 1, 1}); // also delete the latency
        totalLatency_ += latency;
    }

    // 提交指令
    queue_.pop_front();
}

// 时钟周期推进
void Rob::tick() {
    currentCycle_++;
    // 尝试提交指令
    retire();
}
Helper helper{};
CXLController *controller;
Monitors *monitors;

// Simple trim function to remove leading/trailing whitespace.
std::string trim(const std::string &s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(*start))
        start++;
    auto end = s.end();
    do {
        end--;
    } while (std::distance(start, end) > 0 && std::isspace(*end));
    return std::string(start, end + 1);
}

// Parse the full group (a vector of strings) to extract the fetch header info and
// the following instruction lines. If an "O3PipeView:address:" line is present later,
// it will override the address.
InstructionGroup parseGroup(const std::vector<std::string> &group) {
    InstructionGroup ig;
    int fetchIndex = -1;

    // Find the fetch header (first line that starts with "O3PipeView:fetch:")
    for (size_t i = 0; i < group.size(); i++) {
        if (group[i].rfind("O3PipeView:fetch:", 0) == 0) {
            fetchIndex = static_cast<int>(i);
            break;
        }
    }
    if (fetchIndex == -1)
        return ig; // No fetch header found.

    // Parse the fetch header line.
    {
        std::istringstream iss(group[fetchIndex]);
        std::string token;
        std::vector<std::string> tokens;
        while (std::getline(iss, token, ':')) {
            tokens.push_back(token);
        }
        // Expected token layout:
        // tokens[0] = "O3PipeView"
        // tokens[1] = "fetch"
        // tokens[2] = timestamp (e.g., "816000")
        // tokens[3] = address (e.g., "0x7ffff801f7f7")
        // tokens[4] = extra field (often "0")
        // tokens[5] = cycle count (e.g., "1686")
        // There may be an extra empty token if the header line ends with a colon.
        if (tokens.size() >= 6) {
            // ig.address = tokens[3];
            try {
                ig.fetchTimestamp = std::stoull(tokens[2]);
                ig.cycleCount = std::stoull(tokens[5]);
            } catch (...) {
                ig.fetchTimestamp = 0;
                ig.cycleCount = 0;
            }
        }
    }

    // Combine the instruction lines.
    // We assume that the instruction details follow the fetch header until a line starting
    // with "O3PipeView:" is encountered.
    std::string inst;
    for (size_t i = fetchIndex + 1; i < group.size(); i++) {
        // If the line starts with "O3PipeView:" it is a stage indicator.
        // (We still want to check later for an "O3PipeView:address:" line.)
        if (group[i].rfind("O3PipeView:", 0) == 0)
            continue;
        if (!inst.empty())
            inst += " ";
        inst += trim(group[i]);
    }
    ig.instruction = inst;

    ig.address = 0;
    // Look for an override address in any line starting with "O3PipeView:address:".
    for (const auto &line : group) {
        if (line.rfind("O3PipeView:address:", 0) == 0) {
            std::istringstream iss(line);
            std::string token;
            std::vector<std::string> tokens;
            while (std::getline(iss, token, ':')) {
                tokens.push_back(token);
            }
            // Expected tokens:
            // tokens[0] = "O3PipeView"
            // tokens[1] = "address"
            // tokens[2] = the address (e.g., "140737354362576")
            if (tokens.size() >= 3) {
                ig.address = std::stoull(tokens[2]);
            }
        }
    }
    for (const auto &line : group) {
        if (line.rfind("O3PipeView:retire:", 0) == 0) {
            std::istringstream iss(line);
            std::string token;
            std::vector<std::string> tokens;
            while (std::getline(iss, token, ':')) {
                tokens.push_back(token);
            }
            // Expected tokens:
            // tokens[0] = "O3PipeView"
            // tokens[1] = "retire"
            // tokens[2] = the timestamp (e.g., "820000")
            // tokens[3] = the store or load (e.g., "store" or "load")
            // tokens[4] = the timestamp (e.g., "0x7ffff801f7f7")
            if (tokens.size() >= 5) {
                ig.retireTimestamp = std::stoull(tokens[2]);
            }
        }
    }
    return ig;
}

int main(int argc, char *argv[]) {
    spdlog::cfg::load_env_levels();
    cxxopts::Options options("CXLMemSim", "For simulation of CXL.mem Type 3 on Xeon 6");
    options.add_options()("t,target", "The script file to execute",
                          cxxopts::value<std::string>()->default_value("/trace.out"))(
        "h,help", "Help for CXLMemSimRoB", cxxopts::value<bool>()->default_value("false"))(
        "o,topology", "The newick tree input for the CXL memory expander topology",
        cxxopts::value<std::string>()->default_value("(1,(2,3))"))(
        "e,capacity", "The capacity vector of the CXL memory expander with the first local",
        cxxopts::value<std::vector<int>>()->default_value("0,20,20,20"))(
        "m,mode", "Page mode or cacheline mode", cxxopts::value<std::string>()->default_value("cacheline"))(
        "f,frequency", "The frequency for the running thread", cxxopts::value<double>()->default_value("4000"))(
        "l,latency", "The simulated latency by epoch based calculation for injected latency",
        cxxopts::value<std::vector<int>>()->default_value("100,150,100,150,100,150"))(
        "b,bandwidth", "The simulated bandwidth by linear regression",
        cxxopts::value<std::vector<int>>()->default_value("50,50,50,50,50,50"));

    auto result = options.parse(argc, argv);
    if (result["help"].as<bool>()) {
        std::cout << options.help() << std::endl;
        exit(0);
    }
    auto target = result["target"].as<std::string>();
    auto capacity = result["capacity"].as<std::vector<int>>();
    auto latency = result["latency"].as<std::vector<int>>();
    auto bandwidth = result["bandwidth"].as<std::vector<int>>();
    auto topology = result["topology"].as<std::string>();
    page_type mode;
    if (result["mode"].as<std::string>() == "hugepage_2M") {
        mode = HUGEPAGE_2M;
    } else if (result["mode"].as<std::string>() == "hugepage_1G") {
        mode = HUGEPAGE_1G;
    } else if (result["mode"].as<std::string>() == "cacheline") {
        mode = CACHELINE;
    } else {
        mode = PAGE;
    }
    auto *policy = new InterleavePolicy();

    for (auto const &[idx, value] : capacity | std::views::enumerate) {
        if (idx == 0) {
            SPDLOG_DEBUG("local_memory_region capacity:{}", value);
            controller = new CXLController(policy, capacity[0], mode, 100);
        } else {
            SPDLOG_DEBUG("memory_region:{}", (idx - 1) + 1);
            SPDLOG_DEBUG(" capacity:{}", capacity[(idx - 1) + 1]);
            SPDLOG_DEBUG(" read_latency:{}", latency[(idx - 1) * 2]);
            SPDLOG_DEBUG(" write_latency:{}", latency[(idx - 1) * 2 + 1]);
            SPDLOG_DEBUG(" read_bandwidth:{}", bandwidth[(idx - 1) * 2]);
            SPDLOG_DEBUG(" write_bandwidth:{}", bandwidth[(idx - 1) * 2 + 1]);
            auto *ep = new CXLMemExpander(bandwidth[(idx - 1) * 2], bandwidth[(idx - 1) * 2 + 1],
                                          latency[(idx - 1) * 2], latency[(idx - 1) * 2 + 1], (idx - 1), capacity[idx]);
            controller->insert_end_point(ep);
        }
    }
    controller->construct_topo(topology);
    SPDLOG_INFO("{}", controller->output());
    Rob rob(controller, 512);

    // read from file
    std::ifstream file(target);
    if (!file) {
        std::cerr << "Failed to open " << target << std::endl;
        return 1;
    }

    std::vector<std::string> groupLines;
    std::vector<InstructionGroup> instructions;

    // Read the file line by line using std::ranges::istream_view.
    for (const std::string &line : std::ranges::istream_view<std::string>(file)) {
        // If the line starts with "O3PipeView:fetch:" then it's the start of a new group.
        if (line.rfind("O3PipeView:fetch:", 0) == 0) {
            // If we have an existing group, process it.
            if (!groupLines.empty()) {
                instructions.emplace_back(parseGroup(groupLines));
                if (instructions.back().retireTimestamp == 0) {
                    // auto& back = instructions.back();
                    // std::cout << "throwing out: " << back.address << back.cycleCount << "[]" << back.retireTimestamp
                    // << std::endl;
                    instructions.pop_back();
                }
                // Clear the group for the next one.
                groupLines.clear();
            }
        }
        // Add the current line to the group if it’s not empty.
        if (!line.empty()) {
            groupLines.push_back(line);
        }
    }
    // Process any remaining group.
    if (!groupLines.empty()) {
        instructions.emplace_back(parseGroup(groupLines));
    }

    std::sort(instructions.begin(), instructions.end(),
              [](InstructionGroup &a, InstructionGroup &b) { return a.cycleCount < b.cycleCount; });
    // Now simulate issuing them into the ROB
    for (const auto &instruction : instructions) {
        bool issued = false;
        while (!issued) {
            issued = rob.issue(instruction);
            if (!issued) {
                rob.tick(); // 如果无法发射,推进时钟直到有空间
            }
        }
        rob.tick(); // 正常推进时钟
    }

    // 清空ROB
    while (!rob.queue_.empty()) {
        rob.tick();
    }
    // After processing all groups, call your ROB method.
    int nonMemInstr = std::count_if(instructions.begin(), instructions.end(),
                                [](const InstructionGroup &ins) { return ins.address == 0; });
    SPDLOG_INFO("Non-memory instructions: {}", nonMemInstr);

    std::cout << "Stalls: " << rob.getStallCount() << std::endl;
    std::cout << std::format("{}",*controller)  << std::endl;
    return 0;
}
