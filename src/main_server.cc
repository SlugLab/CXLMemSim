/*
 * CXLMemSim controller - Server Mode
 * A simplified main for server mode without monitoring dependencies
 *
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "cxlcontroller.h"
#include "cxlendpoint.h"
#include "helper.h"
#include "policy.h"
#ifndef SERVER_MODE
#include "monitor.h"
#endif
#include <cerrno>
#include <cxxopts.hpp>
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>
#include <thread>

Helper helper{};
CXLController *controller;
#ifndef SERVER_MODE
Monitors *monitors;
#endif

int main(int argc, char *argv[]) {
    spdlog::cfg::load_env_levels();
    cxxopts::Options options("CXLMemSim Server", "CXL.mem Type 3 Memory Controller Server");
    
    options.add_options()
        ("h,help", "Help for CXLMemSim Server", cxxopts::value<bool>()->default_value("false"))
        ("v,verbose", "Verbose level", cxxopts::value<int>()->default_value("2"))
        ("default_latency", "Default latency", cxxopts::value<size_t>()->default_value("100"))
        ("interleave_size", "Interleave size", cxxopts::value<size_t>()->default_value("256"))
        ("capacity", "Capacity of CXL expander in GB", cxxopts::value<int>()->default_value("2"))
        ("p,port", "Server port", cxxopts::value<int>()->default_value("9999"))
        ("t,topology", "Topology file", cxxopts::value<std::string>()->default_value("topology.txt"));

    auto result = options.parse(argc, argv);
    
    if (result.count("help")) {
        fmt::print("{}\n", options.help());
        return 0;
    }

    int verbose = result["verbose"].as<int>();
    size_t default_latency = result["default_latency"].as<size_t>();
    size_t interleave_size = result["interleave_size"].as<size_t>();
    int capacity = result["capacity"].as<int>();
    int port = result["port"].as<int>();
    std::string topology = result["topology"].as<std::string>();

    // Initialize policies
    std::array<Policy *, 4> policies = {
        new AllocationPolicy(),
        new MigrationPolicy(),
        new InterleavePolicy(),
        new CachingPolicy()
    };

    // Create controller
    controller = new CXLController(policies, capacity, PAGE, 10, default_latency);
    
    SPDLOG_INFO("CXLMemSim Server started on port {}", port);
    SPDLOG_INFO("Topology: {}", topology);
    SPDLOG_INFO("Capacity: {} GB", capacity);
    SPDLOG_INFO("Default latency: {} ns", default_latency);
    
    // In server mode, the controller just provides latency calculations
    // The actual server implementation is in qemu_integration
    
    // Keep the process running
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
    
    return 0;
}