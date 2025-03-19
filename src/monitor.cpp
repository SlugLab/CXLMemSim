/*
 * CXLMemSim monitor
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "monitor.h"
#include "bpftimeruntime.h"
#include <csignal>
#include <ctime>
#include <iostream>
#include <vector>
timespec Monitor::last_delay = {0, 0};

std::vector<pid_t> get_thread_ids(pid_t pid) {
    std::vector<pid_t> thread_ids;

    // 构建task目录路径
    std::string task_dir = "/proc/" + std::to_string(pid) + "/task";

    DIR *dir = opendir(task_dir.c_str());
    if (dir == nullptr) {
        std::cerr << "无法打开目录: " << task_dir << " - " << strerror(errno) << std::endl;
        return thread_ids;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        // 跳过 . 和 ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // 将线程ID添加到结果中
        pid_t tid = std::stoi(entry->d_name);
        thread_ids.push_back(tid);
    }

    closedir(dir);
    return thread_ids;
}

// 为特定线程设置CPU亲和性
bool set_thread_affinity(pid_t tid, int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);

    int result = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);

    if (result != 0) {
        std::cerr << "设置线程 " << tid << " 的CPU亲和性失败: " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}

Monitors::Monitors(int cpu_count, cpu_set_t *use_cpuset) : print_flag(true) {
    mon = std::vector<Monitor>(cpu_count);
    /** Init mon */
    for (int i = 0; i < cpu_count; i++) {
        disable(i);

        // 直接分配第i个可用的CPU
        int available_cpu = -1;
        int count = 0;

        for (int cpuid = 0; cpuid < helper.num_of_cpu(); cpuid++) {
            if (!CPU_ISSET(cpuid, use_cpuset)) {
                if (count == i) {
                    available_cpu = cpuid;
                    break;
                }
                count++;
            }
        }

        if (available_cpu != -1) {
            mon[i].cpu_core = available_cpu;
        } else {
            std::cout << "No available CPU" << std::endl;
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
        if (mon[i].status == MONITOR_OFF) {
            mon[i].run();
        }
    }
}
Monitor *Monitors::get_mon(const int tgid, const int tid) {
    for (auto &i : mon) {
        if (i.tgid == tgid && i.tid == tid) {
            return &i;
        }
    }
    return new Monitor();
}
int Monitors::enable(uint32_t tgid, uint32_t tid, bool is_process, uint64_t pebs_sample_period, int32_t tnum) {
    int target = -1;

    for (int i = 0; i < tnum; i++) {
        if (mon[i].tgid == tgid && mon[i].tid == tid) {
            SPDLOG_DEBUG("already exists");
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
        SPDLOG_DEBUG("All cores are used");
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
            if (tid != tgid) {
                static auto thread_ids = get_thread_ids(tgid);
                tid = thread_ids.back();
                if (tid) {
                    thread_ids.pop_back();
                    std::cout << "set affinity for thread " << tid << std::endl;
                    s = sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset);
                    if (s != 0) {
                        std::cout << "Failed to setaffinity for thread " << tid << std::endl;
                        return -2;
                    }
                }
            } else {
                return -2;
            }
        } else {
            std::cout << "Failed to setaffinity" << std::endl;
        }
    }

    /* init */
    disable(target);
    mon[target].status = MONITOR_ON;
    mon[target].tgid = tgid;
    mon[target].tid = tid; // We can setup the process here
    mon[target].is_process = is_process;

    if (pebs_sample_period) {
        mon[target].bpftime_ctx = new BpfTimeRuntime(tid, "../src/cxlmemsim.json");
        /* pebs start */
        mon[target].pebs_ctx = new PEBS(tgid, pebs_sample_period);
        SPDLOG_DEBUG("{}Process [tgid={}, tid={}]: enable to pebs.", target, mon[target].tgid,
                     mon[target].tid); // multiple tid multiple pid
        mon[target].lbr_ctx = new LBR(tgid, 1000);
        new std::jthread(mon[target].wait, &mon, target);
    }
    SPDLOG_INFO("pid {}[tgid={}, tid={}] monitoring start", target, mon[target].tgid, mon[target].tid);

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
    if (mon[target].lbr_ctx != nullptr) {
        mon[target].lbr_ctx->fd = -1;
        mon[target].lbr_ctx->pid = -1;
        mon[target].lbr_ctx->seq = 0;
        mon[target].lbr_ctx->rdlen = 0;
        mon[target].lbr_ctx->seq = 0;
        mon[target].lbr_ctx->mp = nullptr;
        mon[target].lbr_ctx->sample_period = 0;
    }
    if (mon[target].bpftime_ctx != nullptr) {
        mon[target].bpftime_ctx->tid = -1;
    }
    for (auto &j : mon[target].elem) {
        j.pebs.total = 0;
        j.pebs.llcmiss = 0;
        j.lbr.total = 0;
        j.lbr.tid = 0;
        j.lbr.time = 0;
        j.bpftime.total = 0;
        j.bpftime.va = 0;
        j.bpftime.pa = 0;
        j.bpftime.pid = 0;
        j.bpftime.tid = 0;
    }
}
bool Monitors::check_all_terminated(const uint32_t processes) {
    bool allTerminated = true;

    for (uint32_t i = 0; i < processes; ++i) {
        // Atomic load
        auto st = mon[i].status.load();

        if (st == MONITOR_ON || st == MONITOR_OFF) {
            // We still have an active or paused monitor => not all terminated
            allTerminated = false;
        } else if (st != MONITOR_DISABLE) {
            // Possibly MONITOR_TERMINATED or other final states
            // Attempt to finalize if needed
            if (this->terminate(mon[i].tgid, mon[i].tid, processes) < 0) {
                SPDLOG_ERROR("Failed to terminate monitor");
                exit(1);
            }
        }
    }

    return allTerminated;
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
        delete mon[target].lbr_ctx;
        delete mon[target].bpftime_ctx;

        /* Save end time */
        if (mon[target].end_exec_ts.tv_sec == 0 && mon[target].end_exec_ts.tv_nsec == 0) {
            clock_gettime(CLOCK_MONOTONIC, &mon[i].end_exec_ts);
        }
        /* display results */
        std::cout << std::format("========== Process {}[tgid={}, tid={}] statistics summary ==========\n", target,
                                 mon[target].tgid, mon[target].tid);
        double emulated_time =
            (double)(mon[target].end_exec_ts.tv_sec - mon[target].start_exec_ts.tv_sec) +
            (double)(mon[target].end_exec_ts.tv_nsec - mon[target].start_exec_ts.tv_nsec) / 1000000000;
        std::cout << std::format("emulated time ={}", emulated_time) << std::endl;
        std::cout << std::format("total delay   ={}", mon[target].total_delay) << std::endl;
        std::cout << std::format("PEBS sample total {} {}", mon[target].before->pebs.total,
                                 mon[target].after->pebs.llcmiss)
                  << std::endl;
        std::cout << std::format("LBR sample total {}", mon[target].before->lbr.total) << std::endl;
        std::cout << std::format("bpftime sample total {}", mon[target].before->bpftime.total) << std::endl;
        std::cout << std::format("{}", *controller) << std::endl;
        break;
    }

    return target;
}

void Monitor::stop() { // thread create and proecess create get the pmu
    int ret;

    if (this->is_process) {
        // In case of process, use SIGSTOP.
        SPDLOG_DEBUG("Send SIGSTOP to pid={}", this->tid);
        ret = kill(this->tid, SIGSTOP);
    } else {
        // Use SIGUSR1 instead of SIGSTOP.
        // When the target thread receives SIGUSR1, it must stop until it receives SIGCONT.
        SPDLOG_DEBUG("Send SIGUSR1 to tid={}(tgid={})", this->tid, this->tgid);
        ret = syscall(SYS_tgkill, this->tgid, this->tid, SIGUSR1);
    }

    if (ret == -1) {
        if (errno == ESRCH) {
            // in this case process or process group does not exist.
            // It might be a zombie or has terminated execution.
            this->status = MONITOR_TERMINATED;
        } else if (errno == EPERM) {
            this->status = MONITOR_NOPERMISSION;
            SPDLOG_ERROR("Failed to signal to any of the target processes. Due to does not have permission.  It "
                         "might have wrong result.");
        }
    } else {
        this->status = MONITOR_OFF;
        SPDLOG_DEBUG("Process [{}:{}] is stopped.", this->tgid, this->tid);
    }
}

void Monitor::run() {
    // SPDLOG_INFO("Send SIGCONT to tid={}(tgid={})", this->tid, this->tgid);
    // usleep(10);

    if (syscall(SYS_tgkill, this->tgid, this->tid, SIGCONT) == -1) {
        if (errno == ESRCH) {
            // in this case process or process group does not exist.
            // It might be a zombie or has terminated execution.
            this->status = MONITOR_TERMINATED;
        } else if (errno == EPERM) {
            this->status = MONITOR_NOPERMISSION;
            SPDLOG_ERROR("Failed to signal to any of the target processes. Due to does not have permission.  It "
                         "might have wrong result.");
        } else {
            this->status = MONITOR_UNKNOWN;
            perror("Failed to signal to any of the target processes");
            SPDLOG_ERROR("I'm dying {} {}", this->tgid, this->tid);
        }
    } else {
        this->status = MONITOR_ON;
    }
}

void Monitor::clear_time(timespec *time) {
    time->tv_sec = 0;
    time->tv_nsec = 0;
}

Monitor::Monitor() // which one to hook
    : tgid(0), tid(0), cpu_core(0), status(0), injected_delay({0}), wasted_delay({0}), before(nullptr), after(nullptr),
      total_delay(0), start_exec_ts({0}), end_exec_ts({0}), is_process(false) {

    for (auto &j : this->elem) {
        j.cpus = std::vector<CPUElem>(helper.used_cpu.size());
        j.chas = std::vector<CHAElem>(helper.used_cha.size());
    }
}

static bool check_continue(const timespec wasted_delay, const timespec injected_delay) {
    // This equation for original one. but it causes like 45ms-> 60ms
    // calculated delay : 45ms
    // actual elapsed time : 60ms (default epoch: 20ms)
    if (wasted_delay.tv_sec > injected_delay.tv_sec ||
        (wasted_delay.tv_sec >= injected_delay.tv_sec && wasted_delay.tv_nsec >= injected_delay.tv_nsec)) {
        return true;
    }
    return false;
}

uint64_t operator-(const timespec &lhs, const timespec &rhs) {
    return (lhs.tv_sec - rhs.tv_sec) * 1000000000 + (lhs.tv_nsec - rhs.tv_nsec);
}

timespec operator+(const timespec &lhs, const timespec &rhs) {
    timespec result{};

    if (lhs.tv_nsec + rhs.tv_nsec >= 1000000000L) {
        result.tv_sec = lhs.tv_sec + rhs.tv_sec + 1;
        result.tv_nsec = lhs.tv_nsec - 1000000000L + rhs.tv_nsec;
    } else {
        result.tv_sec = lhs.tv_sec + rhs.tv_sec;
        result.tv_nsec = lhs.tv_nsec + rhs.tv_nsec;
    }

    return result;
}
timespec operator*(const timespec &lhs, const timespec &rhs) {
    timespec result{};

    if (lhs.tv_nsec < rhs.tv_nsec) {
        result.tv_sec = lhs.tv_sec - rhs.tv_sec - 1;
        result.tv_nsec = lhs.tv_nsec + 1000000000L - rhs.tv_nsec;
    } else {
        result.tv_sec = lhs.tv_sec - rhs.tv_sec;
        result.tv_nsec = lhs.tv_nsec - rhs.tv_nsec;
    }

    return result;
}
void Monitor::wait(std::vector<Monitor> *mons, int target) {
    auto &mon = (*mons)[target];
    uint64_t diff_nsec, target_nsec;
    timespec start_ts{}, end_ts{};
    timespec sleep_target{}, wanted_delay{}, interval_target{};
    timespec prev_wanted_delay = mon.wanted_delay;
    // while we're alive
    while ((mon.status == MONITOR_ON || mon.status == MONITOR_OFF)) {
        // figure out our delay
        wanted_delay = mon.wanted_delay;
        sleep_target = start_ts + wanted_delay * prev_wanted_delay;
        target_nsec = wanted_delay - prev_wanted_delay;
        interval_target = end_ts + interval_delay;
        if (mon.bpftime_ctx && mon.bpftime_ctx->updater->get(mon.tid))
            mon.bpftime_ctx->updater->update(mon.tgid, prev_wanted_delay.tv_nsec - mon.wanted_delay.tv_nsec);
        else
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &interval_target, nullptr);
        // start time before we ask them to sleep
        clock_gettime(CLOCK_MONOTONIC, &start_ts);
        mon.stop();
        diff_nsec = 0;

        // until we've waited enough time...
        while (diff_nsec < target_nsec) {
            SPDLOG_DEBUG("[{}:{}][OFF] total: {}", mon.tgid, mon.tid, wanted_delay.tv_nsec);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &sleep_target, nullptr);
            clock_gettime(CLOCK_MONOTONIC, &end_ts);
            diff_nsec = end_ts - start_ts;
        }
        mon.run();
        prev_wanted_delay = wanted_delay;
        clock_gettime(CLOCK_MONOTONIC, &end_ts);
    }
    // SPDLOG_INFO("{}:{}", prev_wanted_delay.tv_sec, prev_wanted_delay.tv_nsec);
}