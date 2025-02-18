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

#ifndef CXLMEMSIM_HELPER_H
#define CXLMEMSIM_HELPER_H

#include "incore.h"
#include "uncore.h"
#include <cstdint>
#include <filesystem>
#include <linux/perf_event.h>
#include <ranges>
#include <spdlog/spdlog.h>
#include <vector>
#include <x86intrin.h>

#define PAGE_SIZE 4096
#define DATA_SIZE PAGE_SIZE
#define MMAP_SIZE (PAGE_SIZE + DATA_SIZE)

#define barrier() _mm_mfence()

/* CPU Models */
enum {
    CPU_MDL_BDX = 63,
    CPU_MDL_SKX = 85,
    CPU_MDL_SPR = 143,
    CPU_MDL_ADL = 151,
    CPU_MDL_LNL = 189,
    CPU_MDL_ARL = 198,
    CPU_MDL_SRF = 201,
    CPU_MDL_END = 0x0ffff
};
class Incore;
class Uncore;
class Helper;

struct PerfConfig {
    std::string path_format_cha_type{};
    std::array<std::tuple<std::string, uint64_t, uint64_t>, 4> cha{};
    std::array<std::tuple<std::string, uint64_t, uint64_t>, 4> cpu{};
};
struct ModelContext {
    uint32_t model{};
    PerfConfig perf_conf;
};

struct EmuCXLLatency {
    double read;
    double write;
};

struct EmuCXLBandwidth {
    double read;
    double write;
};

struct BandwidthPass {
    std::tuple<int, int> all_access;
    uint64_t read_config;
    uint64_t write_config;
};

struct LatencyPass {
    std::tuple<int, int> all_access;
    double dramlatency;
    uint64_t readonly;
    uint64_t writeback;
};

struct CHAElem {
    std::array<uint64_t, 4> cha;
};

struct CPUElem {
    std::array<uint64_t, 4> cpu;
};

struct PEBSElem {
    uint64_t total;
    uint64_t llcmiss;
};

struct LBRElem {
    uint64_t total;
    uint64_t tid;
    uint64_t time;
    uint64_t branch_stack[96];
};

struct BPFTimeRuntimeElem {
    uint64_t total;
    uint64_t va;
    uint64_t pa;
    uint64_t pid;
    uint64_t tid;
};

struct CPUInfo {
    uint32_t max_cpuid;
    uint32_t cpu_family;
    uint32_t cpu_model;
    uint32_t cpu_stepping;
};

struct Elem {
    CPUInfo cpuinfo;
    std::vector<CHAElem> chas;
    std::vector<CPUElem> cpus;
    PEBSElem pebs;
    LBRElem lbr;
    BPFTimeRuntimeElem bpftime;
};

class PMUInfo {
public:
    std::vector<Uncore> chas;
    std::vector<Incore> cpus;
    Helper *helper;
    PMUInfo(pid_t pid, Helper *h, PerfConfig *perf_config);
    ~PMUInfo();
    int start_all_pmcs();
    int stop_all_pmcs();
    int freeze_counters_cha_all() const;
    int unfreeze_counters_cha_all() const;
};

class Helper {
public:
    PerfConfig perf_conf{};
    Helper();
    std::string path;
    int cpu;
    int cha;
    std::vector<int> used_cpu;
    std::vector<int> used_cha;
    int num_of_cpu();
    int num_of_cha();
    static void detach_children();
    static void noop_handler(int);
    static void suspend_handler(int);
    double cpu_frequency();
    PerfConfig detect_model(uint32_t model, const std::vector<std::string> &perf_name,
                            const std::vector<uint64_t> &perf_conf1, const std::vector<uint64_t> &perf_conf2);
};

long perf_event_open(perf_event_attr *event_attr, pid_t pid, int cpu, int group_fd, unsigned long flags);

#endif // CXLMEMSIM_HELPER_H
