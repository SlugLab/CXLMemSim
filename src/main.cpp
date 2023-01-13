//
// Created by victoryang00 on 1/12/23.
//
#include "info.h"
#include "monitor.h"
#include <cxxopts.hpp>

int main(int argc, char *argv[]) {
    cxxopts::Options options("CXL-MEM-Simulator",
                             "For simulation of CXL.mem Type 3 on Broadwell, Skylake, and Shaphire Rapids");
    options.add_options()("t,target", "The script file to execute", cxxopts::value<std::string>())(
        "i,interval", "The value for epoch value", cxxopts::value<int>()->default_value("20"))(
        "c,cpuset", "The CPUSET for CPU to set affinity on",
        cxxopts::value<std::vector<std::string>>()->default_value("0,1"))("p,pebsperiod", "The pebs sample period",
                                                                          cxxopts::value<int>()->default_value("1"))(
        "l,latency", "The simulated latency by epoch based calculation for injected latency",
        cxxopts::value<std::vector<int>>()->default_value("100"))(
        "w,weight", "The filename(s) to process", cxxopts::value<std::vector<int>>()->default_value("4"))(
        "b,bandwidth", "The simulated bandwidth by linear regression",
        cxxopts::value<std::vector<int>>()->default_value("50"));

    auto result = options.parse(argc, argv);
    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        exit(0);
    }
    auto target = result["target"].as<std::string>();
    auto interval = result["interval"].as<int>();
    auto cpuset = result["cpuset"].as<std::vector<std::string>>();
    for (auto &b:cpuset){
        std::cout << b << std::endl;
    }
    auto pebsperiod = result["pebsperiod"].as<int>();
    auto latency = result["latency"].as<std::vector<int>>();
    auto weight = result["weight"].as<std::vector<int>>();
    auto bandwidth = result["bandwidth"].as<std::vector<int>>();
    for (auto &b:bandwidth){
        std::cout << b << std::endl;
    }
    std::cout << target << std::endl;
    Monitor monitor;
    Info info;
    return 0;
}