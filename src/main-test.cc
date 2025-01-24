/*
 * CXLMemSim main
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "cxlendpoint.h"
#include "helper.h"
#include "monitor.h"
#include "policy.h"
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cxxopts.hpp>
#include <iostream>
#include <spdlog/cfg/env.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

Helper helper{};
int main(int argc, char *argv[]) {
    spdlog::cfg::load_env_levels();
    cxxopts::Options options("CXLMemSim", "For simulation of CXL.mem Type 3 on Sapphire Rapids");
    options.add_options()("t,target", "The script file to execute",
                          cxxopts::value<std::string>()->default_value("/usr/bin/sleep 10"))(
        "h,help", "Help for CXLMemSim", cxxopts::value<bool>()->default_value("false"))(
        "i,interval", "The value for epoch value", cxxopts::value<int>()->default_value("1"))(
        "s,source", "Collection Phase or Validation Phase", cxxopts::value<bool>()->default_value("false"))(
        "c,cpuset", "The CPUSET for CPU to set affinity on and only run the target process on those CPUs",
        cxxopts::value<std::vector<int>>()->default_value("0"))("d,dramlatency", "The current platform's dram latency",
                                                                cxxopts::value<double>()->default_value("110"))(
        "p,pebsperiod", "The pebs sample period", cxxopts::value<int>()->default_value("100"))(
        "m,mode", "Page mode or cacheline mode", cxxopts::value<std::string>()->default_value("p"))(
        "o,topology", "The newick tree input for the CXL memory expander topology",
        cxxopts::value<std::string>()->default_value("(1,(2,3))"))(
        "e,capacity", "The capacity vector of the CXL memory expander with the firsgt local",
        cxxopts::value<std::vector<int>>()->default_value("0,20,20,20"))(
        "f,frequency", "The frequency for the running thread", cxxopts::value<double>()->default_value("4000"))(
        "l,latency", "The simulated latency by epoch based calculation for injected latency",
        cxxopts::value<std::vector<int>>()->default_value("100,150,100,150,100,150"))(
        "b,bandwidth", "The simulated bandwidth by linear regression",
        cxxopts::value<std::vector<int>>()->default_value("50,50,50,50,50,50"))(
        "x,pmu_name", "The input for Collected PMU",
        cxxopts::value<std::vector<std::string>>()->default_value(
            "tatal_stall,all_dram_rds,l2stall,snoop_fw_wb,llcl_hits,llcl_miss,null,null"))(
        "y,pmu_config1", "The config0 for Collected PMU",
        cxxopts::value<std::vector<uint64_t>>()->default_value("0x04004a3,0x01b7,0x05005a3,0x205c,0x08d2,0x01d3,0,0"))(
        "z,pmu_config2", "The config1 for Collected PMU",
        cxxopts::value<std::vector<uint64_t>>()->default_value("0,0x63FC00491,0,0,0,0,0,0"))(
        "w,weight", "The weight for Linear Regression",
        cxxopts::value<std::vector<double>>()->default_value("88, 88, 88, 88, 88, 88, 88"))(
        "v,weight_vec", "The weight vector for Linear Regression",
        cxxopts::value<std::vector<double>>()->default_value("400, 800, 1200, 1600, 2000, 2400, 3000"));

    auto result = options.parse(argc, argv);
    if (result["help"].as<bool>()) {
        std::cout << options.help() << std::endl;
        exit(0);
    }
    auto target = result["target"].as<std::string>();
    auto interval = result["interval"].as<int>();
    auto cpuset = result["cpuset"].as<std::vector<int>>();
    auto pebsperiod = result["pebsperiod"].as<int>();
    auto latency = result["latency"].as<std::vector<int>>();
    auto bandwidth = result["bandwidth"].as<std::vector<int>>();
    auto frequency = result["frequency"].as<double>();
    auto topology = result["topology"].as<std::string>();
    auto capacity = result["capacity"].as<std::vector<int>>();
    auto dramlatency = result["dramlatency"].as<double>();
    auto pmu_name = result["pmu_name"].as<std::vector<std::string>>();
    auto pmu_config1 = result["pmu_config1"].as<std::vector<uint64_t>>();
    auto pmu_config2 = result["pmu_config2"].as<std::vector<uint64_t>>();
    auto weight = result["weight"].as<std::vector<double>>();
    auto weight_vec = result["weight_vec"].as<std::vector<double>>();
    auto source = result["source"].as<bool>();
    enum page_type mode;
    if (result["mode"].as<std::string>() == "hugepage_2M") {
        mode = page_type::HUGEPAGE_2M;
    } else if (result["mode"].as<std::string>() == "hugepage_1G") {
        mode = page_type::HUGEPAGE_1G;
    } else if (result["mode"].as<std::string>() == "cacheline") {
        mode = page_type::CACHELINE;
    } else {
        mode = page_type::PAGE;
    }

    auto *policy = new InterleavePolicy();
    CXLController *controller;

    uint64_t use_cpus = 0;
    cpu_set_t use_cpuset;
    CPU_ZERO(&use_cpuset);
    for (auto i : cpuset) {
        if (!use_cpus || use_cpus & 1UL << i) {
            CPU_SET(i, &use_cpuset);
            SPDLOG_DEBUG("use cpuid: {}{}\n", i, use_cpus);
        }
    }

    auto tnum = CPU_COUNT(&use_cpuset);
    auto cur_processes = 0;
    auto ncpu = helper.num_of_cpu();
    auto ncha = helper.num_of_cha();
    SPDLOG_DEBUG("tnum:{}, intrval:{}\n", tnum, interval);
    for (auto const &[idx, value] : weight | std::views::enumerate) {
        SPDLOG_DEBUG("weight[{}]:{}\n", weight_vec[idx], value);
    }
    Monitors monitors{tnum, &use_cpuset};

    for (auto const &[idx, value] : capacity | std::views::enumerate) {
        if (idx == 0) {
            SPDLOG_DEBUG("local_memory_region capacity:{}\n", value);
            controller = new CXLController(policy, capacity[0], mode, interval, &monitors);
        } else {
            SPDLOG_DEBUG("memory_region:{}\n", (idx - 1) + 1);
            SPDLOG_DEBUG(" capacity:{}\n", capacity[(idx - 1) + 1]);
            SPDLOG_DEBUG(" read_latency:{}\n", latency[(idx - 1) * 2]);
            SPDLOG_DEBUG(" write_latency:{}\n", latency[(idx - 1) * 2 + 1]);
            SPDLOG_DEBUG(" read_bandwidth:{}\n", bandwidth[(idx - 1) * 2]);
            SPDLOG_DEBUG(" write_bandwidth:{}\n", bandwidth[(idx - 1) * 2 + 1]);
            auto *ep = new CXLMemExpander(bandwidth[(idx - 1) * 2], bandwidth[(idx - 1) * 2 + 1],
                                          latency[(idx - 1) * 2], latency[(idx - 1) * 2 + 1], (idx - 1), capacity[idx]);
            controller->insert_end_point(ep);
        }
    }
    controller->construct_topo(topology);
    SPDLOG_INFO("{}", controller->output());

    SPDLOG_DEBUG("cpu_freq:{}\n", frequency);
    SPDLOG_DEBUG("num_of_cha:{}\n", ncha);
    SPDLOG_DEBUG("num_of_cpu:{}\n", ncpu);
    for (auto j : cpuset) {
        helper.used_cpu.push_back(cpuset[j]);
        helper.used_cha.push_back(cpuset[j]);
    }

    /** Reinterpret the input for the argv argc */
    char cmd_buf[1024] = {0};
    strncpy(cmd_buf, target.c_str(), sizeof(cmd_buf));
    char *strtok_state = nullptr;
    char *filename = strtok_r(cmd_buf, " ", &strtok_state);
    char *args[32] = {nullptr};
    args[0] = filename;
    size_t current_arg_idx;
    for (current_arg_idx = 1; current_arg_idx < 32; ++current_arg_idx) {
        char *current_arg = strtok_r(nullptr, " ", &strtok_state);
        if (current_arg == nullptr) {
            break;
        }
        args[current_arg_idx] = current_arg;
        SPDLOG_INFO("args[{}] = {}\n", current_arg_idx, args[current_arg_idx]);
    }

    /** Create target process */
    Helper::detach_children();
    auto t_process = fork();
    if (t_process < 0) {
        SPDLOG_ERROR("Fork: failed to create target process");
        exit(1);
    }
    if (t_process == 0) {
        execv(filename, args); // taskset in lpace
        SPDLOG_ERROR("Exec: failed to create target process\n");
        exit(1);
    }
    /** In case of process, use SIGSTOP. */
    if (auto res = monitors.enable(t_process, t_process, true, pebsperiod, tnum); res == -1) {
        SPDLOG_ERROR("Failed to enable monitor\n");
        exit(0);
    } else if (res < 0) {
        SPDLOG_DEBUG("pid({}) not found. might be already terminated.\n", t_process);
    }
    cur_processes++;
    SPDLOG_DEBUG("pid of CXLMemSim = {}, cur process={}\n", t_process, cur_processes);

    if (cur_processes >= ncpu) {
        SPDLOG_ERROR("Failed to execute. The number of processes/threads of the target application is more than "
                     "physical CPU cores.\n");
        exit(0);
    }

    /** Wait all the target processes until emulation process initialized. */
    monitors.stop_all(cur_processes);

    /** Get CPU information */
    if (!get_cpu_info(&monitors.mon[0].before->cpuinfo)) {
        SPDLOG_DEBUG("Failed to obtain CPU information.\n");
    }
    auto perf_config =
        helper.detect_model(monitors.mon[0].before->cpuinfo.cpu_model, pmu_name, pmu_config1, pmu_config2);
    PMUInfo pmu{t_process, &helper, &perf_config};

    /*% Caculate epoch time */
    timespec waittime{};
    waittime.tv_sec = interval / 1000;
    waittime.tv_nsec = (interval % 1000) * 1000000;

    SPDLOG_DEBUG("The target process starts running.\n");
    SPDLOG_DEBUG("set nano sec = {}\n", waittime.tv_nsec);
    SPDLOG_TRACE("{}\n", monitors);
    monitors.print_flag = false;

    /* read CHA params */
    for (const auto &mon : monitors.mon) {
        for (auto const &[idx, value] : pmu.chas | std::views::enumerate) {
            pmu.chas[idx].read_cha_elems(&mon.before->chas[idx]);
        }
        for (auto const &[idx, value] : pmu.cpus | std::views::enumerate) {
            pmu.cpus[idx].read_cpu_elems(&mon.before->cpus[idx]);
        }
    }

    uint32_t diff_nsec = 0;
    struct timespec start_ts{}, end_ts{};
    struct timespec sleep_start_ts{}, sleep_end_ts{};

    /** Wait all the target processes until emulation process initialized. */
    monitors.run_all(cur_processes);
    for (int i = 0; i < cur_processes; i++) {
        clock_gettime(CLOCK_MONOTONIC, &monitors.mon[i].start_exec_ts);
    }

    LBR lbr(t_process, 1);
    LBRElem data;
    uint64_t last_frame;
    lbr.start();
    while (true) {
        /** Get from the CXLMemSimHook */
        int n;
        /** Here was a definition for the multi process and thread to enable multiple monitor */

        timespec req = waittime;
        timespec rem = {0};
        timeval cur_time = {0};
        timeval last_time = {0};

        gettimeofday(&last_time, NULL);
        monitors.stop_all(cur_processes);
        while (true) {
            auto ret = nanosleep(&req, &rem);
            if (ret == 0) { // success
                break;
            }
            if (errno == EINTR) {
                SPDLOG_ERROR("nanosleep: remain time {}.{}(sec)\n", (long)rem.tv_sec, (long)rem.tv_nsec);
                // if the milisecs was set below 5, will trigger stop before the target process stop.
                // The pause has been interrupted by a signal that was delivered to the thread.
                req = rem; // call nanosleep() again with the remain time.
                break;
            } else {
                // fatal error
                SPDLOG_ERROR("Failed to wait nanotime");
                exit(0);
            }
        }
        monitors.run_all(cur_processes);

        gettimeofday(&cur_time, NULL);
        if (1 == lbr.read(controller, &data)) {
            int miss_total = 0, cycle_total = 0;
            // calc_time(&data, &waittime, &last_frame);
            for (int i = 2; i < 96; i += 3) {
                int miss_count = (data.branch_stack[i] >> 30) & 0x3;
                int cycle_count = (data.branch_stack[i] >> 4) & 0xffff; // todo check offset
                SPDLOG_INFO("Entry {} has {:x} {:x} info {:x}, counter {}", i / 3, data.branch_stack[i - 2],
                            data.branch_stack[i - 1], data.branch_stack[i], miss_count);
                miss_total += miss_count;
                cycle_total += cycle_count;
            }
            int elapsed = (cur_time.tv_sec - last_time.tv_sec) * 1000000 + (cur_time.tv_usec - last_time.tv_usec);
            int factor = 333 * elapsed / cycle_total; // todo actual frequency calc instead of 333
            waittime.tv_sec = 0;
            waittime.tv_nsec = miss_total * 250 * factor;
            last_time = cur_time; // should also add in waittime
        }

        if (monitors.check_all_terminated(cur_processes)) {
            break;
        }
    } // End while-loop for emulation
    lbr.stop();
    return 0;
}
