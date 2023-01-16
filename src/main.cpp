//
// Created by victoryang00 on 1/12/23.
//
#include "helper.h"
#include "logging.h"
#include "monitor.h"
#include "policy.h"
#include <cerrno>
#include <cinttypes>
#include <clocale>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cxxopts.hpp>
#include <fcntl.h>
#include <getopt.h>
#include <range/v3/view.hpp>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define SOCKET_PATH "/tmp/cxl_mem_simulator.sock"

int main(int argc, char *argv[]) {
    Helper helper;
    cxxopts::Options options("CXL-MEM-Simulator",
                             "For simulation of CXL.mem Type 3 on Broadwell, Skylake, and Saphire Rapids");
    options.add_options()("t,target", "The script file to execute", cxxopts::value<std::string>()->default_value("ls"))(
        "h,help", "The value for epoch value", cxxopts::value<bool>()->default_value("false"))(
        "i,interval", "The value for epoch value", cxxopts::value<int>()->default_value("20"))(
        "c,cpuset", "The CPUSET for CPU to set affinity on", cxxopts::value<std::vector<int>>()->default_value("0,1"))(
        "p,pebsperiod", "The pebs sample period", cxxopts::value<int>()->default_value("1"))(
        "m,mode", "Page mode or cacheline mode", cxxopts::value<std::string>()->default_value("p"))(
        "to,topology", "The newick tree input for the CXL memory expander topology", cxxopts::value<std::string>()->default_value("(1)"))(
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
    auto topology = result["topology"].as<std::string>();

    LOG(DEBUG) << fmt::format("tnum:{}, intrval:{}\n", CPU_COUNT(&use_cpuset), interval);
    for (auto const &[idx, value] : weight | ranges::views::enumerate) {

        LOG(DEBUG) << fmt::format("memory_region:{}\n", idx);
        LOG(DEBUG) << fmt::format(" read_latency:{}\n", latency[idx * 2]);
        LOG(DEBUG) << fmt::format(" write_latency:{}\n", latency[idx * 2 + 1]);
        LOG(DEBUG) << fmt::format(" weight:{}\n", value);
        LOG(DEBUG) << fmt::format(" bandwidth:{}\n", bandwidth[idx]);
    }
    int sock;
    struct sockaddr_un addr {};
    InterleavePolicy policy{topology};

    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);
    remove(addr.sun_path);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        LOG(ERROR) << "Failed to execute. Can't bind to a socket.";
        exit(1);
    }
    LOG(DEBUG) << fmt::format("cpu_freq:{}\n", frequency);
    // LOG(DEBUG) << fmt::format("num_of_cbo:{}\n", frequency);
    Monitors monitors{CPU_COUNT(&use_cpuset), &use_cpuset, static_cast<int>(weight.size()), helper};

    return 0;
}