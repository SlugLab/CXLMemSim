/*
 * CXLMemSim main
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_OFF
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
CXLController *controller;
Monitors *monitors;
auto cha_mapping = std::vector{0, 1, 2, 3, 4, 5, 6, 7, 8};
int main(int argc, char *argv[]) {
    spdlog::cfg::load_env_levels();
    cxxopts::Options options("CXLMemSim", "For simulation of CXL.mem Type 3 on Xeon 6");
    options.add_options()("t,target", "The script file to execute",
                          cxxopts::value<std::string>()->default_value("./microbench/malloc"))(
        "h,help", "Help for CXLMemSim", cxxopts::value<bool>()->default_value("false"))(
        "c,cpuset", "The CPUSET for CPU to set affinity on and only run the target process on those CPUs",
        cxxopts::value<std::vector<int>>()->default_value("0,1,2,3"))(
        "d,dramlatency", "The current platform's dram latency", cxxopts::value<double>()->default_value("110"))(
        "p,pebsperiod", "The pebs sample period", cxxopts::value<int>()->default_value("10"))(
        "m,mode", "Page mode or cacheline mode", cxxopts::value<std::string>()->default_value("p"))(
        "o,topology", "The newick tree input for the CXL memory expander topology",
        cxxopts::value<std::string>()->default_value("(1,(2,3))"))(
        "q,capacity", "The capacity vector of the CXL memory expander with the first local",
        cxxopts::value<std::vector<int>>()->default_value("0,20,20,20"))(
        "f,frequency", "The frequency for the running thread", cxxopts::value<double>()->default_value("4000"))(
        "l,latency", "The simulated latency by epoch based calculation for injected latency",
        cxxopts::value<std::vector<int>>()->default_value("200,250,200,250,200,250"))(
        "b,bandwidth", "The simulated bandwidth by linear regression",
        cxxopts::value<std::vector<int>>()->default_value("50,50,50,50,50,50"))(
        "x,pmu_name", "The input for Collected PMU",
        cxxopts::value<std::vector<std::string>>()->default_value(
            "prefetch,l2hit,l2miss,cpus_dram_rds,llcl_hits,snoop_fwd_wb,total_stall,l2stall"))(
        "y,pmu_config1", "The config0 for Collected PMU",
        cxxopts::value<std::vector<uint64_t>>()->default_value(
            "0x02c0,0x0134,0x7e35,0x01d3,0x04d1,0x01b7,0x04004a3,0x0449"))(
        "z,pmu_config2", "The config1 for Collected PMU",
        cxxopts::value<std::vector<uint64_t>>()->default_value("0,0,0,0,0,0x1a610008,0,0"))(
        "w,weight", "The weight for Linear Regression",
        cxxopts::value<std::vector<double>>()->default_value("88, 88, 88, 88, 88, 88, 88"))(
        "v,weight_vec", "The weight vector for Linear Regression",
        cxxopts::value<std::vector<double>>()->default_value("400, 800, 1200, 1600, 2000, 2400, 3000"))(
        "k,policy", "The policy of CXL memory controller",
        cxxopts::value<std::vector<std::string>>()->default_value("none,none,none,none"))(
        "e,env", "The environment variable for the CXL memory controller",
        cxxopts::value<std::vector<std::string>>()->default_value("OMP_NUM_THREADS=24"));
    ;

    auto result = options.parse(argc, argv);
    if (result["help"].as<bool>()) {
        std::cout << options.help() << std::endl;
        exit(0);
    }
    auto target = result["target"].as<std::string>();
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
    auto page_ = result["mode"].as<std::string>();
    auto policy = result["policy"].as<std::vector<std::string>>();
    auto env = result["env"].as<std::vector<std::string>>();

    page_type mode;
    if (page_ == "hugepage_2M") {
        mode = HUGEPAGE_2M;
    } else if (page_ == "hugepage_1G") {
        mode = HUGEPAGE_1G;
    } else if (page_ == "cacheline") {
        mode = CACHELINE;
    } else {
        mode = PAGE;
    }
    AllocationPolicy *policy1;
    MigrationPolicy *policy2;
    PagingPolicy *policy3;
    CachingPolicy *policy4;

    // 初始化分配策略
    // Initialize allocation policy
    if (policy[0] == "interleave") {
        policy1 = new InterleavePolicy();
    } else if (policy[0] == "numa") {
        policy1 = new NUMAPolicy();
    } else {
        policy1 = new AllocationPolicy();
    }

    // 初始化迁移策略
    // Initialize migration policy
    if (policy[1] == "heataware") {
        policy2 = new HeatAwareMigrationPolicy();
    } else if (policy[1] == "frequency") {
        policy2 = new FrequencyBasedMigrationPolicy();
    } else if (policy[1] == "loadbalance") {
        policy2 = new LoadBalancingMigrationPolicy();
    } else if (policy[1] == "locality") {
        policy2 = new LocalityBasedMigrationPolicy();
    } else if (policy[1] == "lifetime") {
        policy2 = new LifetimeBasedMigrationPolicy();
    } else if (policy[1] == "hybrid") {
        auto *hybridPolicy = new HybridMigrationPolicy();
        // 可以添加多个策略到混合策略中
        // Can add multiple policies to the hybrid policy
        hybridPolicy->add_policy(new HeatAwareMigrationPolicy());
        hybridPolicy->add_policy(new FrequencyBasedMigrationPolicy());
        policy2 = hybridPolicy;
    } else {
        SPDLOG_ERROR("Unknown migration policy: {}", policy[1]);
        policy2 = new MigrationPolicy();
    }

    // 初始化分页策略
    // Initialize paging policy
    if (policy[2] == "hugepage") {
        policy3 = new HugePagePolicy();
    } else if (policy[2] == "pagetableaware") {
        policy3 = new PageTableAwarePolicy();
    } else {
        SPDLOG_ERROR("Unknown paging policy: {}", policy[2]);
        policy3 = new PagingPolicy();
    }

    // 初始化缓存策略
    // Initialize caching policy
    if (policy[3] == "fifo") {
        policy4 = new FIFOPolicy();
    } else if (policy[3] == "frequency") {
        policy4 = new FrequencyBasedInvalidationPolicy();
    } else {
        SPDLOG_ERROR("Unknown caching policy: {}", policy[3]);
        policy4 = new CachingPolicy();
    }

    uint64_t use_cpus = 0;
    cpu_set_t use_cpuset;
    CPU_ZERO(&use_cpuset);
    for (auto i : cpuset) {
        if (!use_cpus || use_cpus & 1UL << i) {
            CPU_SET(i, &use_cpuset);
            std::cout << "use cpuid: " << i << " " << use_cpus << std::endl;
        }
    }

    auto tnum = CPU_COUNT(&use_cpuset);
    auto cur_processes = 0;
    auto ncpu = helper.num_of_cpu();
    auto ncha = helper.num_of_cha();
    SPDLOG_DEBUG("tnum:{}", tnum);
    for (auto const &[idx, value] : weight | std::views::enumerate) {
        SPDLOG_DEBUG("weight[{}]:{}", weight_vec[idx], value);
    }

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
                                          latency[(idx - 1) * 2], latency[(idx - 1) * 2 + 1], idx - 1, capacity[idx]);
            controller->insert_end_point(ep);
        }
    }
    controller->construct_topo(topology);
    /** Hove been got by socket if it's not main thread and synchro */
    SPDLOG_DEBUG("cpu_freq:{}", frequency);
    SPDLOG_DEBUG("num_of_cha:{}", ncha);
    SPDLOG_DEBUG("num_of_cpu:{}", ncpu);
    for (auto j : cpuset) {
        helper.used_cpu.push_back(cpuset[j]);
        helper.used_cha.push_back(cpuset[j]);
    }
    monitors = new Monitors{tnum, &use_cpuset};

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
        SPDLOG_INFO("args[{}] = {}", current_arg_idx, args[current_arg_idx]);
    }

    /** Create target process */
    Helper::detach_children();
    auto t_process = fork();
    if (t_process < 0) {
        SPDLOG_ERROR("Fork: failed to create target process");
        exit(1);
    }
    if (t_process == 0) {
        sleep(1);
        std::vector<const char *> envp;
        envp.emplace_back("LD_PRELOAD=/root/.bpftime/libbpftime-agent.so");
        envp.emplace_back("OMP_NUM_THREADS=4");
        while (!env.empty()) {
            envp.emplace_back(env.back().c_str());
            env.pop_back();
        }
        envp.emplace_back(nullptr);
        execve(filename, args, const_cast<char *const *>(envp.data()));
        SPDLOG_ERROR("Exec: failed to create target process");
        exit(1);
    }
    /** In case of process, use SIGSTOP. */
    if (auto res = monitors->enable(t_process, t_process, true, pebsperiod, tnum); res == -1) {
        SPDLOG_ERROR("Failed to enable monitor");
        exit(0);
    } else if (res < 0) {
        SPDLOG_DEBUG("pid({}) not found. might be already terminated.", t_process);
    }
    cur_processes++;
    SPDLOG_DEBUG("pid of CXLMemSim = {}, cur process={}", t_process, cur_processes);

    if (cur_processes >= ncpu) {
        SPDLOG_ERROR("Failed to execute. The number of processes/threads of the target application is more than "
                     "physical CPU cores.");
        exit(0);
    }

    /** Wait all the target processes until emulation process initialized. */
    monitors->stop_all(cur_processes);

    /** Get CPU information */
    if (!get_cpu_info(&monitors->mon[0].before->cpuinfo)) {
        SPDLOG_DEBUG("Failed to obtain CPU information.");
    }
    auto perf_config =
        helper.detect_model(monitors->mon[0].before->cpuinfo.cpu_model, pmu_name, pmu_config1, pmu_config2);
    PMUInfo pmu{t_process, &helper, &perf_config};

    SPDLOG_DEBUG("The target process starts running.");
    SPDLOG_TRACE("{}", *monitors);
    monitors->print_flag = false;

    /* read CHA params */
    for (const auto &mon : monitors->mon) {
        for (auto const &[idx, value] : pmu.chas | std::views::enumerate) {
            pmu.chas[idx].read_cha_elems(&mon.before->chas[idx]);
        }
        for (auto const &[idx, value] : pmu.cpus | std::views::enumerate) {
            pmu.cpus[idx].read_cpu_elems(&mon.before->cpus[idx]);
        }
    }

    uint32_t diff_nsec = 0;
    timespec start_ts{}, end_ts{};

    /** Wait all the target processes until emulation process initialized. */
    monitors->run_all(cur_processes);
    for (int i = 0; i < cur_processes; i++) {
        clock_gettime(CLOCK_MONOTONIC, &monitors->mon[i].start_exec_ts);
    }

    while (true) {
        uint64_t calibrated_delay;
        for (auto const &[i, mon] : monitors->mon | std::views::enumerate) {
            // check other process
            auto m_status = mon.status.load();
            if (m_status == MONITOR_DISABLE) {
                continue;
            }
            if (m_status == MONITOR_ON || m_status == MONITOR_SUSPEND) {
                clock_gettime(CLOCK_MONOTONIC, &start_ts);
                SPDLOG_DEBUG("[{}:{}:{}] start_ts: {}.{}", i, mon.tgid, mon.tid, start_ts.tv_sec, start_ts.tv_nsec);
                /** Read CHA values */
                std::vector<uint64_t> cha_vec{0, 0, 0, 0}, cpu_vec{0, 0, 0, 0};

                /*** read CPU params */
                uint64_t wb_cnt = 0, target_l2stall = 0, target_llcmiss = 0, target_llchits = 0, target_l2miss = 0,
                         all_llcmiss = 0, all_prefetch = 0;
                double writeback_latency;
                /* read BPFTIMERUNTIME sample */
                if (mon.is_process) {
                    if (mon.bpftime_ctx->read(controller, &mon.after->bpftime) < 0) {
                        SPDLOG_ERROR("[{}:{}:{}] Warning: Failed BPFTIMERUNTIME read", i, mon.tgid, mon.tid);
                    }

                    /* read PEBS sample */
                    if (mon.pebs_ctx->read(controller, &mon.after->pebs) < 0) {
                        SPDLOG_ERROR("[{}:{}:{}] Warning: Failed PEBS read", i, mon.tgid, mon.tid);
                    }
                    /* read LBR sample */
                    if (mon.lbr_ctx->read(controller, &mon.after->lbr) < 0) {
                        SPDLOG_ERROR("[{}:{}:{}] Warning: Failed LBR read", i, mon.tgid, mon.tid);
                    }
                }
                target_llcmiss = mon.after->pebs.total - mon.before->pebs.total;

                for (auto const &[idx, value] : pmu.cpus | std::views::enumerate) {
                    value.read_cpu_elems(&mon.after->cpus[i]);
                    cpu_vec[idx] = mon.after->cpus[i].cpu[idx] - mon.before->cpus[i].cpu[idx];
                }

                for (auto const &[idx, value] : pmu.chas | std::views::enumerate) {
                    value.read_cha_elems(&mon.after->chas[cha_mapping[i]]);
                    cha_vec[idx] = mon.after->chas[cha_mapping[i]].cha[idx] - mon.before->chas[cha_mapping[i]].cha[idx];
                }
                target_llchits = cpu_vec[0];
                wb_cnt = cpu_vec[1];
                target_l2stall = cpu_vec[3];
                target_l2miss = cha_vec[2];
                for (const auto &mon : monitors->mon) {
                    all_llcmiss += mon.after->pebs.total - mon.before->pebs.total;
                    all_prefetch += mon.after->chas[cha_mapping[i]].cha[0] - mon.before->chas[cha_mapping[i]].cha[0];
                }
                auto avg_weight = std::accumulate(weight.begin(), weight.end(), 0.0) / weight.size();
                // LSU
                writeback_latency = (double)target_l2stall * avg_weight *
                                    (wb_cnt * target_llcmiss / (all_llcmiss + all_prefetch + 1) /
                                     (target_llchits + avg_weight * target_llcmiss + 1));
                uint64_t emul_delay =
                    (controller->latency_lat + controller->bandwidth_lat + writeback_latency) * 1000000;

                SPDLOG_DEBUG("[{}:{}:{}] pebs: total={}, ", i, mon.tgid, mon.tid, mon.after->pebs.total);

                mon.before->pebs.total = mon.after->pebs.total;
                mon.before->lbr.total = mon.after->lbr.total;
                mon.before->bpftime.total = mon.after->bpftime.total;

                SPDLOG_DEBUG("delay={}", emul_delay);

                /* compensation of delay END(1) */
                clock_gettime(CLOCK_MONOTONIC, &end_ts);
                diff_nsec += (end_ts.tv_sec - start_ts.tv_sec) * 1000000000 + (end_ts.tv_nsec - start_ts.tv_nsec);
                SPDLOG_DEBUG("diff_nsec:{}", diff_nsec);

                calibrated_delay = diff_nsec > emul_delay ? 0 : emul_delay - diff_nsec;
                mon.total_delay += (double)calibrated_delay / 1000000000;
                diff_nsec = 0;

                /* insert emulated NVM latency */
                mon.injected_delay.tv_sec += std::lround(calibrated_delay / 1000000000);
                mon.injected_delay.tv_nsec += std::lround(calibrated_delay % 1000000000);
                SPDLOG_DEBUG("[{}:{}:{}]delay:{} , total delay:{}", i, mon.tgid, mon.tid, calibrated_delay,
                             mon.total_delay);

                {
                    std::lock_guard lock(mon.wanted_delay_mutex);
                    timespec new_wanted = mon.wanted_delay;
                    new_wanted.tv_nsec += emul_delay;
                    new_wanted.tv_sec += new_wanted.tv_nsec / 1000000000;
                    new_wanted.tv_nsec = new_wanted.tv_nsec % 1000000000;
                    mon.wanted_delay = new_wanted;
                    SPDLOG_DEBUG("{}:{}", new_wanted.tv_sec, new_wanted.tv_nsec);
                    SPDLOG_DEBUG("{}", *monitors);
                }
                controller->latency_lat = 0;
                controller->bandwidth_lat = 0;
            }

        } // End for-loop for all target processes

        if (monitors->check_all_terminated(tnum)) {
            break;
        }
    } // End while-loop for emulation

    return 0;
}
