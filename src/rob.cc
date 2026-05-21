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
#include <cctype>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <queue>
#include <ranges>
#include <spdlog/cfg/env.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

Helper helper{};
CXLController *controller;
Monitors *monitors;

namespace {

struct RobToolOptions {
    bool help = false;
    std::string target = "/trace.out";
    std::string topology = "(1,(2,3))";
    double dramlatency = 110.0;
    std::vector<int> capacity{0, 20, 20, 20};
    std::string mode = "cacheline";
    double frequency = 4000.0;
    std::vector<int> latency{100, 100, 100, 100, 100, 100};
    std::vector<int> bandwidth{50, 50, 50, 50, 50, 50};
    std::string output = "delayed_trace.out";
    int interim_save = 0;
};

std::string trim_copy(const std::string &value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch); });
    const auto end =
        std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch); }).base();
    return begin < end ? std::string(begin, end) : std::string();
}

std::vector<std::string> split_csv(const std::string &value) {
    std::vector<std::string> fields;
    std::istringstream stream(value);
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(trim_copy(field));
    }
    if (fields.empty() && !value.empty()) {
        fields.push_back(trim_copy(value));
    }
    return fields;
}

template <typename T> T parse_value(const std::string &value);

template <> int parse_value<int>(const std::string &value) { return std::stoi(value, nullptr, 0); }

template <> double parse_value<double>(const std::string &value) { return std::stod(value); }

template <> std::string parse_value<std::string>(const std::string &value) { return value; }

template <typename T> std::vector<T> parse_csv_vector(const std::string &value) {
    std::vector<T> parsed;
    for (const auto &field : split_csv(value)) {
        if (!field.empty()) {
            parsed.push_back(parse_value<T>(field));
        }
    }
    return parsed;
}

bool option_value_follows(int argc, char *argv[], int index) { return index + 1 < argc && argv[index + 1][0] != '-'; }

std::string require_cli_value(int argc, char *argv[], int &index, const std::string &key,
                              const std::string &inline_value, bool has_inline_value) {
    if (has_inline_value) {
        return inline_value;
    }
    if (!option_value_follows(argc, argv, index)) {
        throw std::invalid_argument("Missing value for option " + key);
    }
    index++;
    return argv[index];
}

void print_rob_help(const char *program) {
    std::cout << "Usage: " << program << " [options]\n\n"
              << "Options:\n"
              << "  -h, --help                    Show this help text\n"
              << "  -t, --target <trace>          O3PipeView trace input file\n"
              << "  -o, --topology <tree>         Newick-style CXL topology\n"
              << "  -d, --dramlatency <ns>        Platform DRAM latency\n"
              << "  -e, --capacity <list>         Local and expander capacity vector\n"
              << "  -m, --mode <mode>             page, cacheline, hugepage_2M, or hugepage_1G\n"
              << "  -f, --frequency <MHz>         CPU frequency used by the model\n"
              << "  -l, --latency <list>          Read/write latency vector\n"
              << "  -b, --bandwidth <list>        Read/write bandwidth vector\n"
              << "      --output <trace>          Delayed trace output file\n"
              << "      --interim-save <count>    Append interim trace output every N instructions\n";
}

bool parse_rob_options(int argc, char *argv[], RobToolOptions &opts, std::string &error) {
    try {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            std::string key;
            std::string value;
            bool has_inline_value = false;

            if (arg.rfind("--", 0) == 0) {
                auto eq_pos = arg.find('=');
                key = arg.substr(2, eq_pos == std::string::npos ? std::string::npos : eq_pos - 2);
                if (eq_pos != std::string::npos) {
                    value = arg.substr(eq_pos + 1);
                    has_inline_value = true;
                }
            } else if (arg == "-h") {
                key = "help";
            } else if (arg == "-t") {
                key = "target";
            } else if (arg == "-o") {
                key = "topology";
            } else if (arg == "-d") {
                key = "dramlatency";
            } else if (arg == "-e") {
                key = "capacity";
            } else if (arg == "-m") {
                key = "mode";
            } else if (arg == "-f") {
                key = "frequency";
            } else if (arg == "-l") {
                key = "latency";
            } else if (arg == "-b") {
                key = "bandwidth";
            } else {
                throw std::invalid_argument("Unknown option: " + arg);
            }

            auto get_value = [&](const std::string &option_name) {
                return require_cli_value(argc, argv, i, option_name, value, has_inline_value);
            };

            if (key == "help") {
                opts.help = true;
            } else if (key == "target") {
                opts.target = get_value(key);
            } else if (key == "topology") {
                opts.topology = get_value(key);
            } else if (key == "dramlatency") {
                opts.dramlatency = parse_value<double>(get_value(key));
            } else if (key == "capacity") {
                opts.capacity = parse_csv_vector<int>(get_value(key));
            } else if (key == "mode") {
                opts.mode = get_value(key);
            } else if (key == "frequency") {
                opts.frequency = parse_value<double>(get_value(key));
            } else if (key == "latency") {
                opts.latency = parse_csv_vector<int>(get_value(key));
            } else if (key == "bandwidth") {
                opts.bandwidth = parse_csv_vector<int>(get_value(key));
            } else if (key == "output") {
                opts.output = get_value(key);
            } else if (key == "interim-save") {
                opts.interim_save = parse_value<int>(get_value(key));
            } else {
                throw std::invalid_argument("Unknown option: --" + key);
            }
        }
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

} // namespace

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

    // Create parsing thread pool
    const int numThreads = 4;
    std::vector<std::thread> parseThreads;
    std::mutex resultsMutex;

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

    // Start consumer threads
    for (int i = 0; i < numThreads; ++i) {
        parseThreads.emplace_back(parseWorker);
    }

    //  -
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

    // Process the last group
    if (!groupLines.empty()) {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            groupsQueue.push(std::move(groupLines));
        }
        cv.notify_one();
    }

    // Notify all consumer threads to complete
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        done = true;
    }
    cv.notify_all();

    // Wait for all threads to complete
    for (auto &thread : parseThreads) {
        thread.join();
    }

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

    RobToolOptions opts;
    std::string parse_error;
    if (!parse_rob_options(argc, argv, opts, parse_error)) {
        std::cerr << "Failed to parse options: " << parse_error << "\n\n";
        print_rob_help(argv[0]);
        return 1;
    }
    if (opts.help) {
        print_rob_help(argv[0]);
        return 0;
    }

    auto target = opts.target;
    auto capacity = opts.capacity;
    auto latency = opts.latency;
    auto bandwidth = opts.bandwidth;
    auto topology = opts.topology;
    auto dramlatency = opts.dramlatency;
    auto outputTraceFile = opts.output;
    auto interimSaveInterval = opts.interim_save;
    page_type mode;
    if (opts.mode == "hugepage_2M") {
        mode = HUGEPAGE_2M;
    } else if (opts.mode == "hugepage_1G") {
        mode = HUGEPAGE_1G;
    } else if (opts.mode == "cacheline") {
        mode = CACHELINE;
    } else {
        mode = PAGE;
    }
    auto *policy1 = new InterleavePolicy();
    auto *policy2 = new MigrationPolicy();
    auto *policy3 = new PagingPolicy();
    auto *policy4 = new CachingPolicy();

    for (size_t idx = 0; idx < capacity.size(); idx++) {
        if (idx == 0) {
            SPDLOG_DEBUG("local_memory_region capacity:{}", capacity[idx]);
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
    for (size_t idx = 0; idx < instructions.size(); idx++) {
        bool issued = false;
        while (!issued) {
            issued = rob.issue(instructions[idx]);
            if (!issued) {
                rob.tick(); // If unable to issue, advance clock until space is available
            }
        }
        rob.tick(); // Normal clock advancement

        // Save interim results if requested
        if (interimSaveInterval > 0 && idx > 0 && idx % interimSaveInterval == 0) {
            SPDLOG_INFO("Saving interim trace at instruction {}", idx);
            rob.saveInstructionTrace(instructions, outputTraceFile, true);
        }

        if (idx % 10000 == 0) {
            SPDLOG_INFO("Processing instruction {}", idx);
        }
    }

    // ROB
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
