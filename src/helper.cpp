/*
 * CXLMemSim helper
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "helper.h"
#include <fstream>
#if CXLMEMSIM_HAS_LINUX_PERF
#include <monitor.h>
#endif
#include <signal.h>
#include <stdexcept>
#include <string>
#include <vector>

ModelContext model_ctx[] = {{CPU_MDL_BDX,
                             {
                                 "/sys/bus/event_source/devices/uncore_cbo_%u/type",
                             }},
                            {CPU_MDL_SKX,
                             {
                                 "/sys/bus/event_source/devices/uncore_cha_%u/type",
                             }},
                            {CPU_MDL_SPR,
                             {
                                 "/sys/bus/event_source/devices/uncore_cha_%u/type",
                             }},
                            {CPU_MDL_GNR,
                             {
                                 "/sys/bus/event_source/devices/uncore_cha_%u/type",
                             }},
                            {CPU_MDL_SRF,
                             {
                                 "/sys/bus/event_source/devices/uncore_cha_%u/type",
                             }},

                            {CPU_MDL_ADL,
                             {
                                 "/sys/bus/event_source/devices/uncore_cbo_%u/type",
                             }},
                            {CPU_MDL_CFL,
                             {
                                 "/sys/bus/event_source/devices/uncore_cbox_%u/type",
                             }},
                            {CPU_MDL_LNL,
                             {
                                 "/sys/bus/event_source/devices/uncore_cbox_%u/type",
                             }},
                            {CPU_MDL_ARL,
                             {
                                 "/sys/bus/event_source/devices/uncore_cbox_%u/type",
                             }},
                            {CPU_MDL_END, {""}}};

#if CXLMEMSIM_HAS_LINUX_PERF
long perf_event_open(struct perf_event_attr *event_attr, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, event_attr, pid, cpu, group_fd, flags);
}
#endif

int Helper::num_of_cpu() {
    int ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 0) {
        SPDLOG_ERROR("sysconf");
    }
    return ncpu;
}

int Helper::num_of_cha() {
    int ncha = 0;
    for (; ncha < cpu; ++ncha) {
        char path[100];
        std::sprintf(path, this->path.c_str(), ncha);
        if (!std::filesystem::exists(path)) {
            break;
        }
    }
    return ncha;
}

double Helper::cpu_frequency() {
    int i, c = 0;
    double cpu_mhz = 0.0;
    double max_cpu_mhz = 0.0;
    std::ifstream fp("/proc/cpuinfo");

    for (std::string line; c != this->num_of_cpu() - 1; std::getline(fp, line)) {
        SPDLOG_DEBUG("line: {}\n", line);
        i = std::sscanf(line.c_str(), "cpu MHz : %lf", &cpu_mhz);
        max_cpu_mhz = i == 1 ? std::max(max_cpu_mhz, cpu_mhz) : max_cpu_mhz;
        std::sscanf(line.c_str(), "processor : %d", &c);
    }
    SPDLOG_DEBUG("cpu MHz: {}\n", cpu_mhz);

    return cpu_mhz;
}
PerfConfig Helper::detect_model(uint32_t model, const std::vector<std::string> &perf_name,
                                const std::vector<uint64_t> &perf_conf1, const std::vector<uint64_t> &perf_conf2) {
    int i = 0;
    SPDLOG_INFO("Detecting model...{}", model);
    while (model_ctx[i].model != CPU_MDL_END) {
        if (model_ctx[i].model == model) {
            this->perf_conf = model_ctx[i].perf_conf;
            for (size_t j = 0; j < this->perf_conf.cha.size(); ++j) {
                const std::string name = j < perf_name.size() ? perf_name[j] : "cha_pmu_" + std::to_string(j);
                const uint64_t config = j < perf_conf1.size() ? perf_conf1[j] : 0;
                const uint64_t config1 = j < perf_conf2.size() ? perf_conf2[j] : 0;
                this->perf_conf.cha[j] = std::make_tuple(name, config, config1);
            }
            for (size_t j = 0; j < this->perf_conf.cpu.size(); ++j) {
                const size_t src_idx = j + this->perf_conf.cha.size();
                const std::string name =
                    src_idx < perf_name.size() ? perf_name[src_idx] : "cpu_pmu_" + std::to_string(j);
                const uint64_t config = src_idx < perf_conf1.size() ? perf_conf1[src_idx] : 0;
                const uint64_t config1 = src_idx < perf_conf2.size() ? perf_conf2[src_idx] : 0;
                this->perf_conf.cpu[j] = std::make_tuple(name, config, config1);
            }
            this->path = model_ctx[i].perf_conf.path_format_cha_type;
            return this->perf_conf;
        }
        i++;
    }
    SPDLOG_WARN("CPU model {} is not in the Intel CHA table. CHA counters are disabled; CPU PMU counters will use "
                "the supplied raw configs when perf accepts them.",
                model);
    this->perf_conf = {};
    for (size_t j = 0; j < this->perf_conf.cha.size(); ++j) {
        const std::string name = j < perf_name.size() ? perf_name[j] : "cha_pmu_" + std::to_string(j);
        const uint64_t config = j < perf_conf1.size() ? perf_conf1[j] : 0;
        const uint64_t config1 = j < perf_conf2.size() ? perf_conf2[j] : 0;
        this->perf_conf.cha[j] = std::make_tuple(name, config, config1);
    }
    for (size_t j = 0; j < this->perf_conf.cpu.size(); ++j) {
        const size_t src_idx = j + this->perf_conf.cha.size();
        const std::string name = src_idx < perf_name.size() ? perf_name[src_idx] : "cpu_pmu_" + std::to_string(j);
        const uint64_t config = src_idx < perf_conf1.size() ? perf_conf1[src_idx] : 0;
        const uint64_t config1 = src_idx < perf_conf2.size() ? perf_conf2[src_idx] : 0;
        this->perf_conf.cpu[j] = std::make_tuple(name, config, config1);
    }
    this->path.clear();
    return this->perf_conf;
}
Helper::Helper() {
    cpu = num_of_cpu();
    cha = num_of_cha();
}
void Helper::noop_handler(int) { ; }
void Helper::suspend_handler(int) {
#if CXLMEMSIM_HAS_LINUX_PERF
    if (!monitors) {
        return;
    }
    for (auto &m : monitors->mon)
        m.status = MONITOR_SUSPEND;
#endif
}
void Helper::detach_children() {
    struct sigaction sa{};

    sa.sa_handler = noop_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDWAIT;
    if (sigaction(SIGCHLD, &sa, nullptr) < 0) {
        SPDLOG_ERROR("Failed to sigaction: %s", strerror(errno));
    }
    sa.sa_handler = suspend_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGINT, &sa, nullptr) < 0) {
        SPDLOG_ERROR("Failed to sigaction: %s", strerror(errno));
    }
}
int PMUInfo::start_all_pmcs() {
#if !CXLMEMSIM_HAS_LINUX_PERF
    return 0;
#else
    /* enable all pmcs to count */
    int r, i;
    for (i = 0; i < this->cpus.size(); i++) {
        r = this->cpus[i].start();
        if (r < 0) {
            SPDLOG_ERROR("start failed. cpu:{}\n", i);
            return r;
        }
    }
    return 0;
#endif
}
PMUInfo::PMUInfo(pid_t pid, Helper *helper, PerfConfig *perf_config) : helper(helper) {
#if CXLMEMSIM_HAS_LINUX_PERF
    if (!perf_config->path_format_cha_type.empty()) {
        for (auto i : helper->used_cha) {
            try {
                this->chas.emplace_back(i, perf_config);
            } catch (const std::exception &e) {
                SPDLOG_WARN("Skipping CHA PMU {}: {}", i, e.what());
            }
        }
        // unfreeze counters
        int r = this->unfreeze_counters_cha_all();
        if (r < 0) {
            SPDLOG_WARN("unfreeze_counters_cha_all failed; CHA counters are disabled.");
            this->chas.clear();
        }
    } else {
        SPDLOG_WARN("CHA PMU path is unavailable for this CPU; CHA counters are disabled.");
    }

    for (auto i : helper->used_cpu) {
        try {
            this->cpus.emplace_back(pid, i, perf_config);
        } catch (const std::exception &e) {
            SPDLOG_WARN("Skipping CPU PMU {}: {}", i, e.what());
        }
    }

    int r = this->start_all_pmcs();
    if (r < 0) {
        SPDLOG_ERROR("start_all_pmcs failed\n");
    }
#endif
}
int PMUInfo::stop_all_pmcs() {
#if !CXLMEMSIM_HAS_LINUX_PERF
    return 0;
#else
    /* disable all pmcs to count */

    for (int i = 0; i < this->cpus.size(); i++) {
        if (const int r = this->cpus[i].stop(); r < 0) {
            SPDLOG_ERROR("stop failed. cpu:{}\n", i);
            return r;
        }
    }
    return 0;
#endif
}

int PMUInfo::unfreeze_counters_cha_all() const {
#if !CXLMEMSIM_HAS_LINUX_PERF
    return 0;
#else
    for (int i = 0; i < this->chas.size(); i++) {
        for (const int j : {0, 1, 2, 3}) {
            if (int r = this->chas[i].perf[j]->start(); r < 0) {
                SPDLOG_ERROR("perf_start failed. cha:{}\n", i);
                return r;
            }
        }
    }
    return 0;
#endif
}
int PMUInfo::freeze_counters_cha_all() const {
#if !CXLMEMSIM_HAS_LINUX_PERF
    return 0;
#else

    for (int i = 0; i < this->chas.size(); i++) {
        for (const int j : {0, 1, 2, 3}) {
            if (const int r = this->chas[i].perf[j]->stop(); r < 0) {
                SPDLOG_ERROR("perf_stop failed. cha:{}\n", i);
                return r;
            }
        }
    }
    return 0;
#endif
}
PMUInfo::~PMUInfo() {
#if CXLMEMSIM_HAS_LINUX_PERF
    stop_all_pmcs();
    this->cpus.clear();
    this->chas.clear();
#endif
}
