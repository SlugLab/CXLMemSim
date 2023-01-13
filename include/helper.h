//
// Created by victoryang00 on 1/12/23.
//

#ifndef CXL_MEM_SIMULATOR_HELPER_H
#define CXL_MEM_SIMULATOR_HELPER_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* CPU Models */
enum { CPU_MDL_BDX = 63, CPU_MDL_SKX = 85, CPU_MDL_SPR = 143, CPU_MDL_ADL = 151, CPU_MDL_END = 0x0ffff };

struct EmuCXLLatency {
    double read;
    double write;
};

struct EmuCXLBandwidth {
    double read;
    double write;
};

struct PerfInfo {
    int fd;
    int group_fd;
    int cpu;
    pid_t pid;
    unsigned long flags;
    struct perf_event_attr attr;
};

struct CBOElem {
    uint64_t llc_wb;
};

struct CPUElem {
    uint64_t all_dram_rds;
    uint64_t cpu_l2stall_t;
    uint64_t cpu_llcl_hits;
    uint64_t cpu_llcl_miss;
};

struct PEBSElem {
    uint64_t total;
    uint64_t llcmiss;
    uint64_t *sample;
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

struct __incore {
    struct PerfInfo perf[4];
};

struct PMUInfo {

    struct __uncore *cbos;
    struct __incore *cpus;
};

struct RegionInfo {
    uint64_t addr;
    uint64_t size;
};

struct PerfConfig {
    const char *path_format_cbo_type;
    uint64_t cbo_config;
    uint64_t all_dram_rds_config;
    uint64_t all_dram_rds_config1;
    uint64_t cpu_l2stall_config;
    uint64_t cpu_llcl_hits_config;
    uint64_t cpu_llcl_miss_config;
};

struct ModelContext {
    uint32_t model;
    struct PerfConfig perf_conf;
};

class Helper {
    int cpu;
    int cbo;

    Helper() {
        cpu = num_of_cpu();
        cbo = num_of_cbo();
    }
    int num_of_cpu();
    int num_of_cbo();
    double cpu_frequency();
    int detect_model(const uint32_t);
};

#endif // CXL_MEM_SIMULATOR_HELPER_H
