//
// Created by victoryang00 on 1/11/23.
//

#include "monitor.h"
Monitors::Monitors(const int tnum, cpu_set_t *use_cpuset, const int nmem, Helper h) {
    int i, j;
    disable(i);
    mon = std::vector<Monitor>(tnum, Monitor(nmem, h));
    /* init mon */
    for (i = 0; i < tnum; i++) {
        int cpucnt = 0;
        int cpuid = 0;
        for (cpuid = 0; cpuid < h.cpu; cpuid++) {
            if (CPU_ISSET(cpuid, use_cpuset)) {
                if (i == cpucnt) {
                    mon[i].cpu_core = cpuid;
                    break;
                }
                cpucnt++;
            }
        }
    }
}
void Monitors::stop_all(const int processes) {
    for (auto i = 0; i < processes; ++i) {
        if (mon[i].status == MONITOR_ON) {
            mon[i].stop();
        }
    }
}
void Monitors::run_all(const int processes) {
    for (auto i = 0; i < processes; ++i) {
        if (mon[i].status == MONITOR_ON) {
            mon[i].run();
        }
    }
}
int Monitors::enable(const uint32_t tgid, const uint32_t tid, bool is_process, uint64_t pebs_sample_period,
                     const int32_t tnum) {
    int target = -1;

    for (int i = 0; i < tnum; i++) {
        if (mon[i].tgid == tgid && mon[i].tid == tid) {
            LOG(DEBUG) << "already exists";
            return -1;
        }
    }
    for (int i = 0; i < tnum; i++) {
        if (mon[i].status != MONITOR_DISABLE) {
            continue;
        }
        target = i;
        break;
    }
    if (target == -1) {
        LOG(DEBUG) << "All cores are used";
        return -1;
    }

    /* set CPU affinity to not used core. */
    int s;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(mon[target].cpu_core, &cpuset);
    s = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);
    if (s != 0) {
        if (errno == ESRCH) {
            LOG(DEBUG) << fmt::format("Process [{}:{}] is terminated.\n", tgid, tid);
            return -2;
        } else {
            LOG(ERROR) << "Failed to setaffinity";
        }
        throw;
    }

    /* init */
    disable(target);
    mon[target].status = MONITOR_ON;
    mon[target].tgid = tgid;
    mon[target].tid = tid;
    mon[target].is_process = is_process;

    if (pebs_sample_period) {
        /* pebs start */
        mon[target].pebs_ctx = PEBS(tid, pebs_sample_period);
        LOG(DEBUG) << fmt::format("Process [tgid={}, tid={}]: enable to pebs.\n", mon[target].tgid, mon[target].tid);
    }

    std::cout << fmt::format("========== Process {}[tgid={}, tid={}] monitoring start ==========\n", target,
                             mon[target].tgid, mon[target].tid);
}
void Monitors::disable(const uint32_t target) {
    mon[target].is_process = false;
    mon[target].status = MONITOR_DISABLE;
    mon[target].tgid = 0;
    mon[target].tid = 0;
    mon[target].before = &mon[target].elem[0];
    mon[target].after = &mon[target].elem[1];
    mon[target].total_delay = 0;
    mon[target].squabble_delay.tv_sec = 0;
    mon[target].squabble_delay.tv_nsec = 0;
    mon[target].injected_delay.tv_sec = 0;
    mon[target].injected_delay.tv_nsec = 0;
    mon[target].end_exec_ts.tv_sec = 0;
    mon[target].end_exec_ts.tv_nsec = 0;
    mon[target].pebs_ctx.fd = -1;
    mon[target].pebs_ctx.pid = -1;
    mon[target].pebs_ctx.seq = 0;
    mon[target].pebs_ctx.rdlen = 0;
    mon[target].pebs_ctx.seq = 0;
    mon[target].pebs_ctx.mp = nullptr;
    mon[target].pebs_ctx.sample_period = 0;
    for (int i = 0; i < mon[target].num_of_region; i++) {
        for (auto &j : mon[target].elem) {
            j.pebs.sample[i] = 0;
            j.pebs.total = 0;
            j.pebs.llcmiss = 0;
        }
        mon[target].region_info[i].addr = 0;
        mon[target].region_info[i].size = 0;
    }
    mon[target].num_of_region = 0;
}
bool Monitors::check_all_terminated(const uint32_t processes) {
    bool _terminated = true;
    for (uint32_t i = 0; i < processes; ++i) {
        if (mon[i].status == MONITOR_ON || mon[i].status == MONITOR_OFF) {
            _terminated = false;
        } else if (mon[i].status != MONITOR_DISABLE) {
            if (this->terminate(mon[i].tgid, mon[i].tid, processes) < 0) {
                LOG(ERROR) << "Failed to terminate monitor";
                exit(1);
            }
        }
    }
    return _terminated;
}
int Monitors::terminate(const uint32_t tgid, const uint32_t tid, const int32_t tnum) {
    int target = -1;

    for (int i = 0; i < tnum; i++) {
        if (mon[i].status == MONITOR_DISABLE) {
            continue;
        }
        if (mon[i].tgid != tgid || mon[i].tid != tid) {
            continue;
        }
        target = i;
        /* pebs stop */
        mon[target].pebs_ctx.finish();

        /* Save end time */
        if (mon[target].end_exec_ts.tv_sec == 0 && mon[target].end_exec_ts.tv_nsec == 0) {
            clock_gettime(CLOCK_MONOTONIC, &mon[i].end_exec_ts);
        }
        /* display results */
        LOG(INFO) << fmt::format("========== Process {}[tgid={}, tid={}] statistics summary ==========\n", target,
                                 mon[target].tgid, mon[target].tid);
        double emulated_time =
            (double)(mon[target].end_exec_ts.tv_sec - mon[target].start_exec_ts.tv_sec) +
            (double)(mon[target].end_exec_ts.tv_nsec - mon[target].start_exec_ts.tv_nsec) / 1000000000;
        LOG(INFO) << fmt::format("emulated time ={}\n", emulated_time);
        LOG(INFO) << fmt::format("total delay   ={}\n", mon[target].total_delay);
        for (int j; j < mon[target].num_of_region; j++) {
            LOG(INFO) << fmt::format("PEBS sample {} ={}\n", j, mon[target].before->pebs.sample[j]);
        }

        /* init */
        disable(target);
        break;
    }

    return target;
}
bool Monitors::check_continue(const uint32_t target, const struct timespec w) {
    // This equation for original one. but it causes like 45ms-> 60ms
    // calculated delay : 45ms
    // actual elapesed time : 60ms (default epoch: 20ms)
    if (mon[target].wasted_delay.tv_sec > mon[target].injected_delay.tv_sec ||
        (mon[target].wasted_delay.tv_sec >= mon[target].injected_delay.tv_sec &&
         mon[target].wasted_delay.tv_nsec >= mon[target].injected_delay.tv_nsec)) {
        return true;
    }
    return false;
}

int Monitor::set_region_info(const int nreg, struct RegionInfo *ri) {
    int i;

    this->num_of_region = nreg;
    // TODO change to cxlendpoint
    for (i = 0; i < nreg; i++) {
        this->region_info[i].addr = ri[i].addr;
        this->region_info[i].size = ri[i].size;
        LOG(DEBUG) << fmt::format("  region info[{}]: addr={}, size={}\n", i, ri[i].addr, ri[i].size);
    }

    return 0;
}
void Monitor::stop() {
    int ret = -1;

    if (this->is_process) {
        // In case of process, use SIGSTOP.
        LOG(DEBUG) << fmt::format("Send SIGSTOP to pid={}\n", this->tid);
        ret = kill(this->tid, SIGSTOP);
    } else {
        // Use SIGUSR1 instead of SIGSTOP.
        // When the target thread receives SIGUSR1, it must stop until it receives SIGCONT.
        LOG(DEBUG) << fmt::format("Send SIGUSR1 to tid={}(tgid={})\n", this->tid, this->tgid);
        ret = syscall(SYS_tgkill, this->tgid, this->tid, SIGUSR1);
    }

    if (ret == -1) {
        if (errno == ESRCH) {
            // in this case process or process group does not exist.
            // It might be a zombie or has terminated execution.
            this->status = MONITOR_TERMINATED;
            LOG(DEBUG) << fmt::format("Process [{}:{}] is terminated.\n", this->tgid, this->tid);
        } else if (errno == EPERM) {
            this->status = MONITOR_NOPERMISSION;
            LOG(ERROR) << "Failed to signal to any of the target processes. Due to does not have permission. \n It "
                          "might have wrong result.";
        }
    } else {
        this->status = MONITOR_OFF;
        LOG(DEBUG) << fmt::format("Process [{}:{}] is stopped.\n", this->tgid, this->tid);
    }
}
void Monitor::run() {
    LOG(DEBUG) << fmt::format("Send SIGCONT to tid={}(tgid={})\n", this->tid, this->tgid);
    if (syscall(SYS_tgkill, this->tgid, this->tid, SIGCONT) == -1) {
        if (errno == ESRCH) {
            // in this case process or process group does not exist.
            // It might be a zombie or has terminated execution.
            this->status = MONITOR_TERMINATED;
            LOG(DEBUG) << fmt::format("Process [{}:{}] is terminated.\n", this->tgid, this->tid);
        } else if (errno == EPERM) {
            this->status = MONITOR_NOPERMISSION;
            LOG(ERROR) << "Failed to signal to any of the target processes. Due to does not have permission. \n It "
                          "might have wrong result.";
        }
    } else {
        this->status = MONITOR_ON;
        LOG(DEBUG) << fmt::format("Process [{}:{}] is running.\n", this->tgid, this->tid);
    }
}
void Monitor::clear_time(struct timespec *time) {
    time->tv_sec = 0;
    time->tv_nsec = 0;
}
Monitor::Monitor(const int nmem, Helper h)
    : tgid(0), tid(0), cpu_core(0), status(0), injected_delay({0}), wasted_delay({0}), squabble_delay({0}),
      before(nullptr), after(nullptr), total_delay(0), start_exec_ts({0}), end_exec_ts({0}), is_process(false),
      num_of_region(0),  pebs_ctx(0, 0) {
    for (auto &j : this->elem) {
        j.cpus = (struct CPUElem *)calloc(sizeof(struct CPUElem), h.cpu);
        if (j.cpus == nullptr) {
            LOG(ERROR) << "calloc";
            throw;
        }
        j.cbos = (struct CBOElem *)calloc(sizeof(struct CBOElem), h.cbo);
        if (j.cbos == nullptr) {
            LOG(ERROR) << "calloc";
            throw;
        }
        j.pebs.sample = (uint64_t *)calloc(sizeof(uint64_t), nmem);
        if (j.pebs.sample == nullptr) {
            LOG(ERROR) << "calloc";
            throw;
        }
    }
    this->region_info = (struct RegionInfo *)calloc(sizeof(struct RegionInfo), nmem);
    if (this->region_info == nullptr) {
        LOG(ERROR) << "calloc";
        throw;
    }
}
