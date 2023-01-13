//
// Created by victoryang00 on 1/12/23.
//
#include "info.h"
#include "monitor.h"
#include <cxxopts.hpp>

int main(int argc, char *argv[]) {
    cxxopts::Options options("CXL-MEM-Simulator",
                            "For simulation of CXL.mem Type 3 on Broadwell, Skylake, and Shaphire Rapids");
    options.add_options()
            ("target", "The script file to execute", cxxopts::value<std::string>())
            ("targetargs", "The server to execute on", cxxopts::value<std::string>())
            ("interval", "The value for epoch value", cxxopts::value<int>()->default_value("20"))
            ("cpuset", "The CPUSET for CPU to set affinity on", cxxopts::value<std::vector<std::string>>())
            ("pebsperiod", "The pebs sample period", cxxopts::value<std::vector<std::string>>())
            ("latency", "The simulated latency by epoch based calculation for injected latency", cxxopts::value<std::vector<int>>())
            ("weight", "The filename(s) to process", cxxopts::value<std::vector<int>>())
            ("bandwidth", "The simulated bandwidth by linear regression", cxxopts::value<std::vector<int>>())
            ("partition", "The filename(s) to process", cxxopts::value<std::vector<std::string>>());

// Parse options the usual way
    options.parse(argc, argv);
    Monitor monitor;
    Info info;
    return 0;
}