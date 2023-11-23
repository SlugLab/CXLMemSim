//
// Created by victoryang00 on 1/11/23.
//

#include "monitor.h"
Monitors::Monitors(int tnum, cpu_set_t *use_cpuset) : print_flag(true) {
    mon = std::vector<Monitor>(tnum, Monitor());
    /** Init mon */
    for (int i = 0; i < tnum; i++) {
        disable(i);
        int cpucnt = 0, cpuid;
        for (cpuid = 0; cpuid < helper.num_of_cpu(); cpuid++) {
            if (!CPU_ISSET(cpuid, use_cpuset)) {
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
Monitor Monitors::get_mon(int tgid, int tid) {
    for (auto &i : mon) {
        if (i.tgid == tgid && i.tid == tid) {
            return i;
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
    mon[target].tid = tid; // We can setup the process here
    mon[target].is_process = is_process;

    if (pebs_sample_period) {
        /* pebs start */
        mon[target].pebs_ctx = new PEBS(tgid, pebs_sample_period);
        LOG(DEBUG) << fmt::format("{}Process [tgid={}, tid={}]: enable to pebs.\n", target, mon[target].tgid,
                                  mon[target].tid); // multiple tid multiple pid
    }

    LOG(INFO) << fmt::format("pid {}[tgid={}, tid={}] monitoring start\n", target, mon[target].tgid, mon[target].tid);

    return target;
}
void Monitors::disable(const uint32_t target) {
    mon[target].is_process = false; // Here to add the multi process.
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
    if (mon[target].pebs_ctx != nullptr) {
        mon[target].pebs_ctx->fd = -1;
        mon[target].pebs_ctx->pid = -1;
        mon[target].pebs_ctx->seq = 0;
        mon[target].pebs_ctx->rdlen = 0;
        mon[target].pebs_ctx->seq = 0;
        mon[target].pebs_ctx->mp = nullptr;
        mon[target].pebs_ctx->sample_period = 0;
    }
    for (auto &j : mon[target].elem) {
        j.pebs.total = 0;
        j.pebs.llcmiss = 0;
    }
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
        delete mon[target].pebs_ctx;

        /* Save end time */
        if (mon[target].end_exec_ts.tv_sec == 0 && mon[target].end_exec_ts.tv_nsec == 0) {
            clock_gettime(CLOCK_MONOTONIC, &mon[i].end_exec_ts);
        }
        /* display results */
        std::cout << fmt::format("========== Process {}[tgid={}, tid={}] statistics summary ==========\n", target,
                                 mon[target].tgid, mon[target].tid);
        double emulated_time =
            (double)(mon[target].end_exec_ts.tv_sec - mon[target].start_exec_ts.tv_sec) +
            (double)(mon[target].end_exec_ts.tv_nsec - mon[target].start_exec_ts.tv_nsec) / 1000000000;
        std::cout << fmt::format("emulated time ={}\n", emulated_time);
        std::cout << fmt::format("total delay   ={}\n", mon[target].total_delay);

        std::cout << fmt::format("PEBS sample total {}\n", mon[target].before->pebs.total);

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

void Monitor::stop() { // thread create and proecess create get the pmu
    int ret;

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

Monitor::Monitor() // which one to hook
    : tgid(0), tid(0), cpu_core(0), status(0), injected_delay({0}), wasted_delay({0}), squabble_delay({0}),
      before(nullptr), after(nullptr), total_delay(0), start_exec_ts({0}), end_exec_ts({0}), is_process(false),
      pebs_ctx(nullptr) {

    for (auto &j : this->elem) {
        j.cpus = std::vector<CPUElem>(helper.used_cpu.size());
        j.chas = std::vector<CHAElem>(helper.used_cha.size());
    }
}