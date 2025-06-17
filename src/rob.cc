/*
 * CXLMemSim rob file
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "rob.h"
#include "policy.h"
#include <algorithm> // For std::transform
#include <atomic>
#include <cxxopts.hpp>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <queue>
#include <ranges>
#include <spdlog/cfg/env.h>
#include <sstream>
#include <thread>
#include <vector>

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
void parseInParallel(std::ifstream &file, std::vector<InstructionGroup> &instructions) {
    std::mutex queueMutex;
    std::condition_variable cv;
    std::queue<std::vector<std::string>> groupsQueue;
    std::atomic<bool> done{false};
    std::vector<std::string> groupLines;

    // 创建解析线程池
    // Create parsing thread pool
    const int numThreads = 4;
    std::vector<std::thread> parseThreads;
    std::mutex resultsMutex;

    // 消费者线程函数
    // Consumer thread function
    auto parseWorker = [&]() {
        while (true) {
            std::vector<std::string> group;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                cv.wait(lock, [&]() { return !groupsQueue.empty() || done; });
                if (groupsQueue.empty() && done)
                    break;
                group = std::move(groupsQueue.front());
                groupsQueue.pop();
            }

            auto result = parseGroup(group);
            if (result.retireTimestamp != 0) {
                std::lock_guard<std::mutex> lock(resultsMutex);
                instructions.emplace_back(std::move(result));
            }
        }
    };

    // 启动消费者线程
    // Start consumer threads
    for (int i = 0; i < numThreads; ++i) {
        parseThreads.emplace_back(parseWorker);
    }

    // 生产者部分 - 主线程
    // Producer part - main thread
    for (const std::string &line : std::ranges::istream_view<std::string>(file)) {
        if (line.rfind("O3PipeView:fetch:", 0) == 0) {
            if (!groupLines.empty()) {
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    groupsQueue.push(groupLines);
                }
                cv.notify_one();
                groupLines.clear();
            }
        }
        if (!line.empty()) {
            groupLines.push_back(line);
        }
    }

    // 处理最后一组
    // Process the last group
    if (!groupLines.empty()) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            groupsQueue.push(std::move(groupLines));
        }
        cv.notify_one();
    }

    // 通知所有消费者线程完成
    // Notify all consumer threads to complete
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        done = true;
    }
    cv.notify_all();

    // 等待所有线程完成
    // Wait for all threads to complete
    for (auto &thread : parseThreads) {
        thread.join();
    }

    // 排序结果
    // Sort results
    std::sort(instructions.begin(), instructions.end(),
              [](InstructionGroup &a, InstructionGroup &b) { return a.cycleCount < b.cycleCount; });
}

// Generate delayed trace output similar to input
void generateDelayedTrace(const std::vector<InstructionGroup> &instructions, Rob &rob, const std::string &outputFile) {
    std::ofstream outFile(outputFile);
    if (!outFile) {
        std::cerr << "Failed to open output file " << outputFile << std::endl;
        return;
    }

    SPDLOG_INFO("Generating trace with {} instructions", instructions.size());

    // For each instruction, use the updated timestamps from the simulation
    for (const auto &ins : instructions) {
        // Base timestamps from the original fetch timestamp
        long long fetchTS = ins.fetchTimestamp;
        long long decodeTS = fetchTS + 500; // typically +500 cycles from fetch
        long long renameTS = fetchTS + 1000; // typically +1000 cycles from fetch
        long long dispatchTS = fetchTS + 1500; // typically +1500 cycles from fetch
        long long issueTS = fetchTS + 1500; // typically same as dispatch
        long long completeTS = ins.retireTimestamp - 500; // typically completes 500 cycles before retire
        long long retireTS = ins.retireTimestamp;

        // No need to recalculate retire time, as it's been updated during simulation
        // Just need to format the output correctly

        // Write out the trace in the same format as input
        outFile << "O3PipeView:fetch:" << fetchTS << ":" << std::hex << "0x" << ins.address << std::dec
                << ":0:" << ins.cycleCount << ":  " << ins.instruction << std::endl;
        outFile << "O3PipeView:decode:" << decodeTS << std::endl;
        outFile << "O3PipeView:rename:" << renameTS << std::endl;
        outFile << "O3PipeView:dispatch:" << dispatchTS << std::endl;
        outFile << "O3PipeView:issue:" << issueTS << std::endl;
        outFile << "O3PipeView:complete:" << completeTS << std::endl;

        // For memory instructions, include store/load indicator and timestamp
        if (ins.address != 0) {
            // Determine if it's a load or store based on instruction text (more accurate detection)
            std::string memOpType = "store";
            std::string instrLower = ins.instruction;
            std::transform(instrLower.begin(), instrLower.end(), instrLower.begin(), ::tolower);

            if (instrLower.find("ld") != std::string::npos || instrLower.find("load") != std::string::npos ||
                instrLower.find("mov_r_m") != std::string::npos) {
                memOpType = "load";
            }

            // Include access timestamp - using the adjusted retireTS from simulation
            outFile << "O3PipeView:retire:" << retireTS << ":" << memOpType << ":" << (retireTS + 1000)
                    << std::endl; // additional timestamp parameter
            outFile << "O3PipeView:address:" << ins.address << std::endl;
        } else {
            outFile << "O3PipeView:retire:" << retireTS << ":store:0" << std::endl;
        }
    }

    outFile.close();
    SPDLOG_INFO("Trace generation complete: {}", outputFile);
}

int main(int argc, char *argv[]) {
    spdlog::cfg::load_env_levels();
    cxxopts::Options options("CXLMemSim", "For simulation of CXL.mem Type 3 on Xeon 6");
    options.add_options()("t,target", "The script file to execute",
                          cxxopts::value<std::string>()->default_value("/trace.out"))(
        "h,help", "Help for CXLMemSimRoB", cxxopts::value<bool>()->default_value("false"))(
        "o,topology", "The newick tree input for the CXL memory expander topology",
        cxxopts::value<std::string>()->default_value("(1,(2,3))"))(
        "d,dramlatency", "The current platform's dram latency", cxxopts::value<double>()->default_value("110"))(
        "e,capacity", "The capacity vector of the CXL memory expander with the first local",
        cxxopts::value<std::vector<int>>()->default_value("0,20,20,20"))(
        "m,mode", "Page mode or cacheline mode", cxxopts::value<std::string>()->default_value("cacheline"))(
        "f,frequency", "The frequency for the running thread", cxxopts::value<double>()->default_value("4000"))(
        "l,latency", "The simulated latency by epoch based calculation for injected latency",
        cxxopts::value<std::vector<int>>()->default_value("100,100,100,100,100,100"))(
        "b,bandwidth", "The simulated bandwidth by linear regression",
        cxxopts::value<std::vector<int>>()->default_value("50,50,50,50,50,50"))(
        "output", "Output trace file with RoB delays",
        cxxopts::value<std::string>()->default_value("delayed_trace.out"))(
        "interim-save", "Save interim trace results every N instructions", cxxopts::value<int>()->default_value("0"));

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
    auto dramlatency = result["dramlatency"].as<double>();
    auto outputTraceFile = result["output"].as<std::string>();
    auto interimSaveInterval = result["interim-save"].as<int>();
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
    auto *policy1 = new InterleavePolicy();
    auto *policy2 = new MigrationPolicy();
    auto *policy3 = new PagingPolicy();
    auto *policy4 = new CachingPolicy();

    for (auto const &[idx, value] : capacity | std::views::enumerate) {
        if (idx == 0) {
            SPDLOG_DEBUG("local_memory_region capacity:{}", value);
            controller = new CXLController({policy1, policy2, policy3, policy4}, capacity[0], mode, 100, dramlatency);
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
    Rob rob(controller, 512);

    // read from file
    std::ifstream file(target);
    if (!file) {
        std::cerr << "Failed to open " << target << std::endl;
        return 1;
    }
    // std::vector<std::string> groupLines;
    std::vector<InstructionGroup> instructions;
    parseInParallel(file, instructions);
    SPDLOG_INFO("{} instructions to process", instructions.size());

    // Delete any existing output file before starting
    if (interimSaveInterval > 0) {
        std::ofstream clearFile(outputTraceFile, std::ios::trunc);
        clearFile.close();
    }

    // Now simulate issuing them into the ROB
    for (const auto &[idx, instruction] : instructions | std::views::enumerate) {
        bool issued = false;
        while (!issued) {
            issued = rob.issue(instruction);
            if (!issued) {
                rob.tick(); // If unable to issue, advance clock until space is available
            }
        }
        rob.tick();  // Normal clock advancement

        // Save interim results if requested
        if (interimSaveInterval > 0 && idx > 0 && idx % interimSaveInterval == 0) {
            SPDLOG_INFO("Saving interim trace at instruction {}", idx);
            rob.saveInstructionTrace(instructions, outputTraceFile, true);
        }

        if (idx % 10000 == 0) {
            SPDLOG_INFO("Processing instruction {}", idx);
        }
    }

    // 清空ROB
    while (!rob.queue_.empty()) {
        rob.tick();
    }

    // After processing all groups, call the trace generation using the updated instructions
    SPDLOG_INFO("ROB processing complete, generating final trace");

    // Decide whether to append or create a new file based on whether we did interim saves
    if (interimSaveInterval > 0) {
        rob.saveInstructionTrace(instructions, outputTraceFile, true);
    } else {
        // Use the existing function since it's already updated
        generateDelayedTrace(instructions, rob, outputTraceFile);
    }

    // Log statistics
    int nonMemInstr = std::count_if(instructions.begin(), instructions.end(),
                                    [](const InstructionGroup &ins) { return ins.address == 0; });
    SPDLOG_INFO("Non-memory instructions: {}", nonMemInstr);

    std::cout << "Stalls: " << rob.getStallCount() << std::endl;
    std::cout << "ROB Events: " << rob.getStallEventCount() << std::endl;
    std::cout << "Generated delayed trace to: " << outputTraceFile << std::endl;

    std::cout << std::format("{}", *controller) << std::endl;
    return 0;
}
