//
// Created by victoryang00 on 1/12/23.
//

#ifndef CXLMEMSIM_HELPER_H
#define CXLMEMSIM_HELPER_H

#include "incore.h"
#include "logging.h"
#include "uncore.h"
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fnmatch.h>
#include <linux/perf_event.h>
#include <map>
#include <optional>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

/* CPU Models */
enum { CPU_MDL_BDX = 63, CPU_MDL_SKX = 85, CPU_MDL_SPR = 143, CPU_MDL_ADL = 151, CPU_MDL_END = 0x0ffff };
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
    struct PerfConfig perf_conf;
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

struct CPUInfo {
    uint32_t max_cpuid;
    uint32_t cpu_family;
    uint32_t cpu_model;
    uint32_t cpu_stepping;
};

struct Elem {
    struct CPUInfo cpuinfo;
    std::vector<CHAElem> chas;
    std::vector<CPUElem> cpus;
    struct PEBSElem pebs;
};

class PMUInfo {
public:
    std::vector<Uncore> chas;
    std::vector<Incore> cpus;
    Helper *helper;
    PMUInfo(pid_t pid, Helper *h, struct PerfConfig *perf_config);
    ~PMUInfo();
    int start_all_pmcs();
    int stop_all_pmcs();
    int freeze_counters_cha_all();
    int unfreeze_counters_cha_all();
};

class Helper {
public:
    PerfConfig perf_conf{};
    Helper();
    int cpu;
    int cha;
    std::vector<int> used_cpu;
    std::vector<int> used_cha;
    int num_of_cpu();
    int num_of_cha();
    static void detach_children();
    static void noop_handler(int signum);
    double cpu_frequency();
    PerfConfig detect_model(uint32_t model, const std::vector<std::string> &perf_name,
                            const std::vector<uint64_t> &perf_conf1, const std::vector<uint64_t> &perf_conf2);
};

#endif // CXLMEMSIM_HELPER_H
