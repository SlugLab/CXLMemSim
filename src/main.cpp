//
// Created by victoryang00 on 1/12/23.
//
#include "helper.h"
#include "info.h"
#include "logging.h"
#include "monitor.h"
#include "policy.h"
#include <cmath>
#include <cxxopts.hpp>
#include <range/v3/view.hpp>

int main(int argc, char *argv[]) {
    Monitor monitor;
    Info info;
    NaivePolicy policy;
    Helper helper;
    cxxopts::Options options("CXL-MEM-Simulator",
                             "For simulation of CXL.mem Type 3 on Broadwell, Skylake, and Shaphire Rapids");
    options.add_options()("t,target", "The script file to execute", cxxopts::value<std::string>()->default_value("ls"))(
        "h,help", "The value for epoch value", cxxopts::value<bool>()->default_value("false"))(
        "i,interval", "The value for epoch value", cxxopts::value<int>()->default_value("20"))(
        "c,cpuset", "The CPUSET for CPU to set affinity on", cxxopts::value<std::vector<int>>()->default_value("0,1"))(
        "p,pebsperiod", "The pebs sample period", cxxopts::value<int>()->default_value("1"))(
        "f,frequency", "The frequency for the running thread", cxxopts::value<int>()->default_value("4"))(
        "l,latency", "The simulated latency by epoch based calculation for injected latency",
        cxxopts::value<std::vector<int>>()->default_value("100,150"))(
        "w,weight", "The simulated weight for multiplying with the LLC miss",
        cxxopts::value<std::vector<int>>()->default_value("4"))(
        "b,bandwidth", "The simulated bandwidth by linear regression",
        cxxopts::value<std::vector<int>>()->default_value("50"));

    auto result = options.parse(argc, argv);
    if (result["help"].as<bool>()) {
        std::cout << options.help() << std::endl;
        exit(0);
    }
    auto target = result["target"].as<std::string>();
    auto interval = result["interval"].as<int>();
    auto cpuset = result["cpuset"].as<std::vector<int>>();
    uint64_t use_cpus = 0;
    cpu_set_t use_cpuset;
    CPU_ZERO(&use_cpuset);
    for (auto c : cpuset) {
        use_cpus += std::pow(2, c);
    }
    for (int i = 0; i < helper.cpu; i++) {
        if (!use_cpus || use_cpus & 1UL << i) {
            CPU_SET(i, &use_cpuset);
            LOG(DEBUG) << fmt::format("use cpuid: {}\n", i);
        }
    }
    auto pebsperiod = result["pebsperiod"].as<int>();
    auto latency = result["latency"].as<std::vector<int>>();
    auto weight = result["weight"].as<std::vector<int>>();
    auto bandwidth = result["bandwidth"].as<std::vector<int>>();
    auto frequency = result["frequency"].as<int>();

    LOG(DEBUG) << fmt::format("tnum:{}, intrval:{}\n", CPU_COUNT(&use_cpuset), interval);
    for (auto const &[idx, value] : weight | ranges::views::enumerate) {

        LOG(DEBUG) << fmt::format("memory_region:{}\n", idx);
        LOG(DEBUG) << fmt::format(" read_latency:{}\n", latency[idx * 2]);
        LOG(DEBUG) << fmt::format(" write_latency:{}\n", latency[idx * 2 + 1]);
        LOG(DEBUG) << fmt::format(" weight:{}\n", value);
        LOG(DEBUG) << fmt::format(" bandwidth:{}\n", bandwidth[idx]);
    }
    LOG(DEBUG) << fmt::format("cpu_freq:{}\n", frequency);
    // LOG(DEBUG) << fmt::format("num_of_cbo:{}\n", frequency);

    return 0;
}