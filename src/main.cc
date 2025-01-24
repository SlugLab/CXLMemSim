/*
 * CXLMemSim main
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
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
Monitors *monitors;
int main(int argc, char *argv[]) {
    spdlog::cfg::load_env_levels();
    cxxopts::Options options("CXLMemSim", "For simulation of CXL.mem Type 3 on Sapphire Rapids");
    options.add_options()("t,target", "The script file to execute",
                          cxxopts::value<std::string>()->default_value("./microbench/many_malloc"))(
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
            "total_stall,all_dram_rds,l2stall,null,llcl_hits,llcl_miss,snoop_fwd_wb,null"))(
        "y,pmu_config1", "The config0 for Collected PMU",
        cxxopts::value<std::vector<uint64_t>>()->default_value("0x04004a3,0x0022,0x0449,0,0xd104,0x20d1,0x01b7,0"))(
        "z,pmu_config2", "The config1 for Collected PMU",
        cxxopts::value<std::vector<uint64_t>>()->default_value("0,0,0,0,0,0,0x1a610008,0"))(
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

    for (auto const &[idx, value] : capacity | std::views::enumerate) {
        if (idx == 0) {
            SPDLOG_DEBUG("local_memory_region capacity:{}\n", value);
            controller = new CXLController(policy, capacity[0], mode, interval, monitors);
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

    /** Hove been got by socket if it's not main thread and synchro */
    SPDLOG_DEBUG("cpu_freq:{}\n", frequency);
    SPDLOG_DEBUG("num_of_cha:{}\n", ncha);
    SPDLOG_DEBUG("num_of_cpu:{}\n", ncpu);
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
        std::vector<char *> envp;
        sleep(1);
        envp.push_back("LD_PRELOAD=/root/.bpftime/libbpftime-agent.so");
        envp.push_back(nullptr);
        execve(filename, args, envp.data()); // taskset in lpace
        SPDLOG_ERROR("Exec: failed to create target process\n");
        exit(1);
    }
    /** In case of process, use SIGSTOP. */
    if (auto res = monitors->enable(t_process, t_process, true, pebsperiod, tnum); res == -1) {
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
    monitors->stop_all(cur_processes);

    /** Get CPU information */
    if (!get_cpu_info(&monitors->mon[0].before->cpuinfo)) {
        SPDLOG_DEBUG("Failed to obtain CPU information.\n");
    }
    auto perf_config =
        helper.detect_model(monitors->mon[0].before->cpuinfo.cpu_model, pmu_name, pmu_config1, pmu_config2);
    PMUInfo pmu{t_process, &helper, &perf_config};

    /*% Caculate epoch time */
    timespec waittime{};
    waittime.tv_sec = interval / 1000;
    waittime.tv_nsec = (interval % 1000) * 1000000;

    SPDLOG_DEBUG("The target process starts running.\n");
    SPDLOG_DEBUG("set nano sec = {}\n", waittime.tv_nsec);
    SPDLOG_TRACE("{}\n", monitors);
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
    struct timespec start_ts{}, end_ts{};
    struct timespec sleep_start_ts{}, sleep_end_ts{};

    /** Wait all the target processes until emulation process initialized. */
    monitors->run_all(cur_processes);
    for (int i = 0; i < cur_processes; i++) {
        clock_gettime(CLOCK_MONOTONIC, &monitors->mon[i].start_exec_ts);
    }

    while (true) {
        /** Get from the CXLMemSimHook */
        int n;
        /** Here was a definition for the multi process and thread to enable multiple monitor */
        timespec req = waittime;
        timespec rem = {0};
        while (true) {
            auto ret = nanosleep(&req, &rem);
            if (ret == 0) { // success
                break;
            }
            if (errno == EINTR) {
                SPDLOG_INFO("nanosleep: remain time {}.{}(sec)\n", (long)rem.tv_sec, (long)rem.tv_nsec);
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

        uint64_t calibrated_delay;
        for (auto const &[i, mon] : monitors->mon | std::views::enumerate) {
            // check other process
            if (mon.status == MONITOR_DISABLE) {
                continue;
            }
            if (mon.status == MONITOR_ON || mon.status == MONITOR_SUSPEND) {
                clock_gettime(CLOCK_MONOTONIC, &start_ts);
                SPDLOG_DEBUG("[{}:{}:{}] start_ts: {}.{}\n", i, mon.tgid, mon.tid, start_ts.tv_sec, start_ts.tv_nsec);
                mon.stop();
                /** Read CHA values */
                uint64_t wb_cnt = 0;
                std::vector<uint64_t> cha_vec, cpu_vec{};
                SPDLOG_INFO("[{}:{}:{}] LLC_WB = {}\n", i, mon.tgid, mon.tid, wb_cnt);

                for (int j = 0; j < helper.used_cha.size(); j++) {
                    for (auto const &[idx, value] : pmu.chas | std::views::enumerate) {
                        value.read_cha_elems(&mon.after->chas[j]);
                        cha_vec.emplace_back(mon.after->chas[j].cha[idx] - mon.before->chas[j].cha[idx]);
                    }
                }
                /*** read CPU params */
                uint64_t read_config = 0;
                uint64_t target_l2stall = 0, target_llcmiss = 0, target_llchits = 0;

                /* read BPFTIMERUNTIME sample */
                if (mon.bpftime_ctx->read(controller, &mon.after->bpftime) < 0) {
                    SPDLOG_ERROR("[{}:{}:{}] Warning: Failed BPFTIMERUNTIME read\n", i, mon.tgid, mon.tid);
                }
                /* read LBR sample */
                if (mon.lbr_ctx->read(controller, &mon.after->lbr) < 0) {
                    SPDLOG_ERROR("[{}:{}:{}] Warning: Failed LBR read\n", i, mon.tgid, mon.tid);
                }
                /* read PEBS sample */
                if (mon.pebs_ctx->read(controller, &mon.after->pebs) < 0) {
                    SPDLOG_ERROR("[{}:{}:{}] Warning: Failed PEBS read\n", i, mon.tgid, mon.tid);
                }
                target_llcmiss = mon.after->pebs.total - mon.before->pebs.total;

                target_l2stall = mon.after->cpus[mon.cpu_core].cpu[1] - mon.before->cpus[mon.cpu_core].cpu[1];
                target_llchits = mon.after->cpus[mon.cpu_core].cpu[2] - mon.before->cpus[mon.cpu_core].cpu[2];
                for (auto const &[idx, value] : pmu.cpus | std::views::enumerate) {
                    target_l2stall += mon.after->cpus[idx].cpu[1] - mon.before->cpus[idx].cpu[1];
                    target_llchits += mon.after->cpus[idx].cpu[2] - mon.before->cpus[idx].cpu[2];
                }
                for (int j = 0; j < helper.used_cpu.size(); j++) {
                    for (auto const &[idx, value] : pmu.cpus | std::views::enumerate) {
                        value.read_cpu_elems(&mon.after->cpus[j]);
                        wb_cnt = mon.after->cpus[j].cpu[idx] - mon.before->cpus[j].cpu[idx];
                        cpu_vec.emplace_back(mon.after->cpus[j].cpu[idx] - mon.before->cpus[j].cpu[idx]);
                    }
                }
                uint64_t llcmiss_wb = 0;
                // To estimate the number of the writeback-involving LLC
                // misses of the CPU core (llcmiss_wb), the total number of
                // writebacks observed in L3 (wb_cnt) is devided
                // proportionally, according to the number of the ratio of
                // the LLC misses of the CPU core (target_llcmiss) to that
                // of the LLC misses of all the CPU cores and the
                // prefetchers (cpus_dram_rds).
                llcmiss_wb = wb_cnt * std::lround(((double)target_llcmiss) / ((double)read_config));
                uint64_t llcmiss_ro = 0;
                if (target_llcmiss < llcmiss_wb) { // tunning
                    SPDLOG_ERROR("[{}:{}:{}] cpus_dram_rds {}, llcmiss_wb {}, target_llcmiss {}\n", i, mon.tgid,
                                 mon.tid, read_config, llcmiss_wb, target_llcmiss);
                    llcmiss_wb = target_llcmiss;
                    llcmiss_ro = 0;
                } else {
                    llcmiss_ro = target_llcmiss - llcmiss_wb;
                }
                SPDLOG_DEBUG("[{}:{}:{}]llcmiss_wb={}, llcmiss_ro={}\n", i, mon.tgid, mon.tid, llcmiss_wb, llcmiss_ro);

                uint64_t emul_delay = 0;

                SPDLOG_DEBUG("[{}:{}:{}] pebs: total={}, \n", i, mon.tgid, mon.tid, mon.after->pebs.total);

                /** TODO: calculate latency construct the passing value and use interleaving policy and counter to
                 * get the sample_prop */
                auto all_access = controller->get_all_access();
                LatencyPass lat_pass = {
                    .all_access = all_access,
                    .dramlatency = dramlatency,
                    .readonly = llcmiss_ro,
                    .writeback = llcmiss_wb,
                };
                BandwidthPass bw_pass = {
                    .all_access = all_access,
                    .read_config = read_config,
                    .write_config = read_config,
                };
                emul_delay += std::lround(controller->calculate_latency(lat_pass));
                emul_delay += controller->calculate_bandwidth(bw_pass);
                emul_delay += std::get<0>(controller->calculate_congestion());

                mon.before->pebs.total = mon.after->pebs.total;

                SPDLOG_DEBUG("delay={}\n", emul_delay);

                /* compensation of delay END(1) */
                clock_gettime(CLOCK_MONOTONIC, &end_ts);
                diff_nsec += (end_ts.tv_sec - start_ts.tv_sec) * 1000000000 + (end_ts.tv_nsec - start_ts.tv_nsec);
                SPDLOG_DEBUG("dif:{}\n", diff_nsec);

                calibrated_delay = (diff_nsec > emul_delay) ? 0 : emul_delay - diff_nsec;
                mon.total_delay += (double)calibrated_delay / 1000000000;
                diff_nsec = 0;

                /* insert emulated NVM latency */
                mon.injected_delay.tv_sec += std::lround(calibrated_delay / 1000000000);
                mon.injected_delay.tv_nsec += std::lround(calibrated_delay % 1000000000);
                SPDLOG_DEBUG("[{}:{}:{}]delay:{} , total delay:{}\n", i, mon.tgid, mon.tid, calibrated_delay,
                             mon.total_delay);

            } else if (mon.status == MONITOR_OFF) {
                // Wasted epoch time
                clock_gettime(CLOCK_MONOTONIC, &start_ts);
                uint64_t sleep_diff = (sleep_end_ts.tv_sec - sleep_start_ts.tv_sec) * 1000000000 +
                                      (sleep_end_ts.tv_nsec - sleep_start_ts.tv_nsec);
                struct timespec sleep_time{};
                sleep_time.tv_sec = std::lround(sleep_diff / 1000000000);
                sleep_time.tv_nsec = std::lround(sleep_diff % 1000000000);
                mon.wasted_delay.tv_sec += sleep_time.tv_sec;
                mon.wasted_delay.tv_nsec += sleep_time.tv_nsec;
                SPDLOG_DEBUG("[{}:{}:{}][OFF] total: {}| wasted : {}| waittime : {}| squabble : {}\n", i, mon.tgid,
                             mon.tid, mon.injected_delay.tv_nsec, mon.wasted_delay.tv_nsec, waittime.tv_nsec,
                             mon.squabble_delay.tv_nsec);
                if (monitors->check_continue(i, sleep_time)) {
                    Monitor::clear_time(&mon.wasted_delay);
                    Monitor::clear_time(&mon.injected_delay);
                    mon.run();
                }
                clock_gettime(CLOCK_MONOTONIC, &end_ts);
                diff_nsec += (end_ts.tv_sec - start_ts.tv_sec) * 1000000000 + (end_ts.tv_nsec - start_ts.tv_nsec);
            }

            if (mon.status == MONITOR_OFF && mon.injected_delay.tv_nsec != 0) {
                long remain_time = mon.injected_delay.tv_nsec - mon.wasted_delay.tv_nsec;
                /* do we need to get squabble time ? */
                if (mon.wasted_delay.tv_sec >= waittime.tv_sec && remain_time < waittime.tv_nsec) {
                    mon.squabble_delay.tv_nsec += remain_time;
                    if (mon.squabble_delay.tv_nsec < 40000000) {
                        SPDLOG_DEBUG("[SQ]total: {}| wasted : {}| waittime : {}| squabble : {}\n",
                                     mon.injected_delay.tv_nsec, mon.wasted_delay.tv_nsec, waittime.tv_nsec,
                                     mon.squabble_delay.tv_nsec);
                        Monitor::clear_time(&mon.wasted_delay);
                        Monitor::clear_time(&mon.injected_delay);
                        mon.run();
                    } else {
                        mon.injected_delay.tv_nsec += mon.squabble_delay.tv_nsec;
                        Monitor::clear_time(&mon.squabble_delay);
                    }
                }
            }
        } // End for-loop for all target processes
        SPDLOG_TRACE("%s\n", monitors);
        for (auto mon : monitors->mon) {
            if (mon.status == MONITOR_ON) {
                auto swap = mon.before;
                mon.before = mon.after;
                mon.after = swap;

                /* continue suspended processes: send SIGCONT */
                // mon.unfreeze_counters_cha_all(fds.msr[0]);
                // start_pmc(&fds, i);
                if (calibrated_delay == 0) {
                    Monitor::clear_time(&mon.wasted_delay);
                    Monitor::clear_time(&mon.injected_delay);
                    mon.run();
                }
            }
        }
        if (monitors->check_all_terminated(tnum)) {
            break;
        }
    } // End while-loop for emulation

    return 0;
}
