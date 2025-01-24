/*
 * CXLMemSim monitor
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */


#ifndef CXLMEMSIM_MONITOR_H
#define CXLMEMSIM_MONITOR_H

#include "cxlcontroller.h"
#include "helper.h"
#include "pebs.h"
#include "bpftimeruntime.h"
#include <sched.h>
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
    bool check_continue(uint32_t, struct timespec) const;
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
    PEBS *pebs_ctx;
    LBR *lbr_ctx;
    BpfTimeRuntime *bpftime_ctx;
    explicit Monitor();

    void stop();
    void run();
    static void clear_time(struct timespec *);
};


template <>
struct std::formatter<Monitors> {
    // Parse function to handle any format specifiers (if needed)
    constexpr auto parse(std::format_parse_context& ctx) -> decltype(ctx.begin()) {
        // If you have specific format specifiers, parse them here
        // For simplicity, we'll ignore them and return the end iterator
        return ctx.end();
    }

    // Format function to output the Monitors data
    template <typename FormatContext>
    auto format(const Monitors& p, FormatContext& ctx) const -> decltype(ctx.out()) {
        std::string result;

        if (p.print_flag) {
            for (const auto& [mon_id, mon] : p.mon | std::views::enumerate) {
                for (size_t core_idx = 0; core_idx < helper.used_cha.size(); ++core_idx) {
                    for (size_t cha_idx = 0; cha_idx < helper.perf_conf.cha.size(); ++cha_idx) {
                        result += std::format("mon{}_{}_{}_{},", mon_id,
                                              std::get<0>(helper.perf_conf.cha[cha_idx]),
                                              helper.used_cha[core_idx], core_idx);
                    }
                }

                for (size_t core_idx = 0; core_idx < helper.used_cpu.size(); ++core_idx) {
                    for (size_t cpu_idx = 0; cpu_idx < helper.perf_conf.cpu.size(); ++cpu_idx) {
                        bool is_last_cpu = (cpu_idx == helper.perf_conf.cpu.size() - 1);
                        bool is_last_core = (core_idx == helper.used_cpu.size() - 1);
                        if (is_last_cpu && is_last_core) {
                            result += std::format("mon{}_{}_{}_{}", mon_id,
                                                  std::get<0>(helper.perf_conf.cpu[cpu_idx]),
                                                  helper.used_cpu[core_idx], core_idx);
                        } else {
                            result += std::format("mon{}_{}_{}_{},", mon_id,
                                                  std::get<0>(helper.perf_conf.cpu[cpu_idx]),
                                                  helper.used_cpu[core_idx], core_idx);
                        }
                    }
                }
            }
        } else {  // Visitor mode
            for (const auto& [mon_id, mon] : p.mon | std::views::enumerate) {
                for (size_t core_idx = 0; core_idx < helper.used_cha.size(); ++core_idx) {
                    for (size_t cha_idx = 0; cha_idx < helper.perf_conf.cha.size(); ++cha_idx) {
                        int cha_diff = mon.after->chas[core_idx].cha[cha_idx] -
                                       mon.before->chas[core_idx].cha[cha_idx];
                        result += std::format("{},", cha_diff);
                    }
                }

                for (size_t core_idx = 0; core_idx < helper.used_cpu.size(); ++core_idx) {
                    for (size_t cpu_idx = 0; cpu_idx < helper.perf_conf.cpu.size(); ++cpu_idx) {
                        bool is_last_cpu = (cpu_idx == helper.perf_conf.cpu.size() - 1);
                        bool is_last_core = (core_idx == helper.used_cpu.size() - 1);
                        int cpu_diff = mon.after->cpus[core_idx].cpu[cpu_idx] -
                                       mon.before->cpus[core_idx].cpu[cpu_idx];
                        if (is_last_cpu && is_last_core) {
                            result += std::format("{}", cpu_diff);
                        } else {
                            result += std::format("{},", cpu_diff);
                        }
                    }
                }
            }
        }

        // Write the accumulated result to the output iterator
        return std::copy(result.begin(), result.end(), ctx.out());
    }
};
#endif // CXLMEMSIM_MONITOR_H
