//
// Created by victoryang00 on 1/12/23.
//

#ifndef CXL_MEM_SIMULATOR_HELPER_H
#define CXL_MEM_SIMULATOR_HELPER_H

#include "incore.h"
#include "logging.h"
#include "uncore.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fnmatch.h>
#include <linux/perf_event.h>
#include <map>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

/* CPU Models */
enum { CPU_MDL_BDX = 63, CPU_MDL_SKX = 85, CPU_MDL_SPR = 143, CPU_MDL_ADL = 151, CPU_MDL_END = 0x0ffff };
class Incore;
class Uncore;
class Helper;

struct EmuCXLLatency {
    double read;
    double write;
};

struct EmuCXLBandwidth {
    double read;
    double write;
};

struct CBOElem {
    uint64_t llc_wb;
};

struct CPUElem {
    uint64_t all_dram_rds;
    uint64_t cpu_l2stall_t;
    uint64_t cpu_llcl_hits;
    uint64_t cpu_llcl_miss;
    uint64_t cpu_bandwidth_read;
    uint64_t cpu_bandwidth_write;
    std::map<uint64_t, uint64_t> cpu_munmap_address_length;
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
    struct CBOElem *cbos;
    struct CPUElem *cpus;
    struct PEBSElem pebs;
};

struct BandwidthPass {
    std::tuple<int, int> all_access;
    uint64_t read_config;
    uint64_t write_config;
};

struct LatencyPass {
    std::tuple<int, int> all_access;
    double dramlatency;
    double ma_ro;
    double ma_wb;
};

class PMUInfo {
public:
    std::vector<Uncore> cbos;
    std::vector<Incore> cpus;
    Helper *helper;
    PMUInfo(pid_t pid, Helper *h, struct PerfConfig *perf_config);
    ~PMUInfo();
    int start_all_pmcs();
    int stop_all_pmcs();
    int freeze_counters_cbo_all();
    int unfreeze_counters_cbo_all();
};

struct PerfConfig {
    const char *path_format_cbo_type;
    uint64_t cbo_config;
    uint64_t all_dram_rds_config;
    uint64_t all_dram_rds_config1;
    uint64_t cpu_l2stall_config;
    uint64_t cpu_llcl_hits_config;
    uint64_t cpu_llcl_miss_config;
    uint64_t cpu_bandwidth_read_config;
    uint64_t cpu_bandwidth_write_config;
};

struct ModelContext {
    uint32_t model;
    struct PerfConfig perf_conf;
};

class Helper {
public:
    int cpu;
    int cbo;
    double cpu_freq;
    PerfConfig perf_conf;
    Helper();
    static int num_of_cpu();
    static int num_of_cbo();
    static void detach_children();
    static void noop_handler(int signum);
    double cpu_frequency() const;
    PerfConfig detect_model(uint32_t);
};

#endif // CXL_MEM_SIMULATOR_HELPER_H
