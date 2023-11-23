//
// Created by victoryang00 on 1/12/23.
//
#include "helper.h"
#include <string>
#include <vector>

struct ModelContext model_ctx[] = {{CPU_MDL_BDX,
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
                                   {CPU_MDL_ADL,
                                    {
                                        "/sys/bus/event_source/devices/uncore_cbo_%u/type",
                                    }},
                                   {CPU_MDL_END, {""}}};

int Helper::num_of_cpu() {
    int ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 0) {
        LOG(ERROR) << "sysconf";
    }
    return ncpu;
}

int Helper::num_of_cha() {
    int ncha = 0;
    for (; ncha < cpu; ++ncha) {
        std::string path = fmt::format("/sys/bus/event_source/devices/uncore_cha_{}/type", ncha);
        //         LOG(DEBUG) << path;
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
        // LOG(DEBUG) << fmt::format("line: {}\n", line);
        i = std::sscanf(line.c_str(), "cpu MHz : %lf", &cpu_mhz);
        max_cpu_mhz = i == 1 ? std::max(max_cpu_mhz, cpu_mhz) : max_cpu_mhz;
        std::sscanf(line.c_str(), "processor : %d", &c);
    }
    LOG(DEBUG) << fmt::format("cpu MHz: {}\n", cpu_mhz);

    return cpu_mhz;
}
PerfConfig Helper::detect_model(uint32_t model, const std::vector<std::string> &perf_name,
                                const std::vector<uint64_t> &perf_conf1, const std::vector<uint64_t> &perf_conf2) {
    int i = 0;
    LOG(INFO) << fmt::format("Detecting model...{}\n", model);
    while (model_ctx[i].model != CPU_MDL_END) {
        if (model_ctx[i].model == model) {
            this->perf_conf = model_ctx[i].perf_conf;
            for (int j = 0; j < 4; ++j) {
                this->perf_conf.cha[j] = std::make_tuple(perf_name[j], perf_conf1[j], perf_conf2[j]);
            }
            for (int j = 0; j < 4; ++j) {
                this->perf_conf.cpu[j] = std::make_tuple(perf_name[j + 4], perf_conf1[j + 4], perf_conf2[j + 4]);
            }
            return this->perf_conf;
        }
        i++;
    }
    LOG(ERROR) << "Failed to execute. This CPU model is not supported. Refer to perfmon or pcm to add support\n";
    throw;
}
Helper::Helper() {
    cpu = num_of_cpu();
    cha = num_of_cha();
}
void Helper::noop_handler(int sig) { ; }
void Helper::detach_children() {
    struct sigaction sa {};

    sa.sa_handler = noop_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDWAIT;
    if (sigaction(SIGCHLD, &sa, nullptr) < 0) {
        LOG(ERROR) << fmt::format("Failed to sigaction: %s", strerror(errno));
    }
}
int PMUInfo::start_all_pmcs() {
    /* enable all pmcs to count */
    int r, i;
    for (i = 0; i < this->cpus.size(); i++) {
        r = this->cpus[i].start();
        if (r < 0) {
            LOG(ERROR) << fmt::format("start failed. cpu:{}\n", i);
            return r;
        }
    }
    return 0;
}
PMUInfo::PMUInfo(pid_t pid, Helper *helper, struct PerfConfig *perf_config) : helper(helper) {
    int r;

    for (auto i : helper->used_cpu) {
        this->chas.emplace_back(i, perf_config);
    }
    // unfreeze counters
    r = this->unfreeze_counters_cha_all();
    if (r < 0) {
        LOG(DEBUG) << fmt::format("unfreeze_counters_cha_all failed.\n");
        throw;
    }

    for (auto i : helper->used_cpu) {
        this->cpus.emplace_back(pid, i, perf_config);
    }

    r = this->start_all_pmcs();
    if (r < 0) {
        LOG(ERROR) << fmt::format("start_all_pmcs failed\n");
    }
}
int PMUInfo::stop_all_pmcs() {
    /* disable all pmcs to count */
    int i, r;

    for (i = 0; i < this->cpus.size(); i++) {
        r = this->cpus[i].stop();
        if (r < 0) {
            LOG(ERROR) << fmt::format("stop failed. cpu:{}\n", i);
            return r;
        }
    }
    return 0;
}

int PMUInfo::unfreeze_counters_cha_all() {
    int i, r;

    for (i = 0; i < this->chas.size(); i++) {
        for (int j : {0, 1, 2, 3}) {
            r = this->chas[i].perf[j]->start();
            if (r < 0) {
                LOG(ERROR) << fmt::format("perf_start failed. cha:{}\n", i);
                return r;
            }
        }
    }
    return 0;
}
int PMUInfo::freeze_counters_cha_all() {
    int i, r;

    for (i = 0; i < this->chas.size(); i++) {
        for (int j : {0, 1, 2, 3}) {
            r = this->chas[i].perf[j]->stop();
            if (r < 0) {
                LOG(ERROR) << fmt::format("perf_stop failed. cha:{}\n", i);
                return r;
            }
        }
    }
    return 0;
}
PMUInfo::~PMUInfo() {
    this->cpus.clear();
    this->chas.clear();
    stop_all_pmcs();
}
