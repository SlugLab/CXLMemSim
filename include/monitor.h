//
// Created by victoryang00 on 1/11/23.
//

#ifndef CXLMEMSIM_MONITOR_H
#define CXLMEMSIM_MONITOR_H

#include "cxlcontroller.h"
#include "helper.h"
#include "pebs.h"
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <sched.h>
#include <unistd.h>
#include <vector>

enum MONITOR_STATUS {
    MONITOR_OFF = 0,
    MONITOR_ON = 1,
    MONITOR_TERMINATED = 2,
    MONITOR_NOPERMISSION = 3,
    MONITOR_DISABLE = 4,
    MONITOR_SUSPEND = 5,
    MONITOR_UNKNOWN = 0xff
};

extern Helper helper;

class Monitor;
class Monitors {
public:
    std::vector<Monitor> mon;
    bool print_flag;
    Monitors(int tnum, cpu_set_t *use_cpuset);
    ~Monitors() = default;

    void stop_all(int);
    void run_all(int);
    Monitor get_mon(int, int);
    int enable(const uint32_t, const uint32_t, bool, uint64_t, const int32_t);
    void disable(uint32_t target);
    int terminate(uint32_t, uint32_t, int32_t);
    bool check_all_terminated(uint32_t);
    bool check_continue(uint32_t, struct timespec);
};

class Monitor {
public:
    pid_t tgid; // process id
    pid_t tid;
    uint32_t cpu_core;
    char status;
    struct timespec injected_delay; // recorded time for injected
    struct timespec wasted_delay; // recorded time for calling between continue and calculation
    struct timespec squabble_delay; // inj-was
    struct Elem elem[2]; // before & after
    struct Elem *before, *after;
    double total_delay;
    struct timespec start_exec_ts, end_exec_ts;
    bool is_process;
    struct PEBS *pebs_ctx;

    explicit Monitor();

    void stop();
    void run();
    static void clear_time(struct timespec *);
};

template <> struct fmt::formatter<Monitors> {
    fmt::formatter<int> f;

    constexpr auto parse(auto &ctx) { return f.parse(ctx); }

    auto format(Monitors const &p, auto &ctx) const {
        auto out = fmt::format_to(ctx.out(), "");
        if (p.print_flag) {
            for (auto const &[mon_id, mon] : p.mon | enumerate) {
                for (auto core_idx = 0; core_idx < helper.used_cha.size(); core_idx++) {
                    for (auto cha_idx = 0; cha_idx < helper.perf_conf.cha.size(); cha_idx++) {
                        out = fmt::format_to(out, "mon{}_{}_{}_{},", mon_id, std::get<0>(helper.perf_conf.cha[cha_idx]),
                                             helper.used_cha[core_idx], core_idx);
                    }
                }

                for (auto core_idx = 0; core_idx < helper.used_cpu.size(); core_idx++) {
                    for (auto cpu_idx = 0; cpu_idx < helper.perf_conf.cpu.size(); cpu_idx++) {
                        if (cpu_idx == helper.perf_conf.cpu.size() - 1 && core_idx == helper.used_cpu.size() - 1) {
                            out = fmt::format_to(out, "mon{}_{}_{}_{}", mon_id,
                                                 std::get<0>(helper.perf_conf.cpu[cpu_idx]), helper.used_cpu[core_idx],
                                                 core_idx);
                        } else {
                            out = fmt::format_to(out, "mon{}_{}_{}_{},", mon_id,
                                                 std::get<0>(helper.perf_conf.cpu[cpu_idx]), helper.used_cpu[core_idx],
                                                 core_idx);
                        }
                    }
                }
            }
        } else {

            for (auto const &[mon_id, mon] : p.mon | enumerate) {
                for (auto core_idx = 0; core_idx < helper.used_cha.size(); core_idx++) {
                    for (auto cha_idx = 0; cha_idx < helper.perf_conf.cha.size(); cha_idx++) {
                        out = fmt::format_to(out, "{},",
                                             mon.after->chas[core_idx].cha[cha_idx] -
                                                 mon.before->chas[core_idx].cha[cha_idx]);
                    }
                }
                for (auto core_idx = 0; core_idx < helper.used_cpu.size(); core_idx++) {
                    for (auto cpu_idx = 0; cpu_idx < helper.perf_conf.cpu.size(); cpu_idx++) {
                        if (cpu_idx == helper.perf_conf.cpu.size() - 1 && core_idx == helper.used_cpu.size() - 1) {
                            out = fmt::format_to(out, "{}",
                                                 mon.after->cpus[core_idx].cpu[cpu_idx] -
                                                     mon.before->cpus[core_idx].cpu[cpu_idx]);
                        } else {
                            out = fmt::format_to(out, "{},",
                                                 mon.after->cpus[core_idx].cpu[cpu_idx] -
                                                     mon.before->cpus[core_idx].cpu[cpu_idx]);
                        }
                    }
                }
            } // visitor mode write to the file
        }
        //        *out++ = '\n';
        ctx.advance_to(out);
        return out;
    };
};

#endif // CXLMEMSIM_MONITOR_H
