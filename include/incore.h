// Created by victoryang00 on 1/14/23.
//

#ifndef CXL_MEM_SIMULATOR_INCORE_H
#define CXL_MEM_SIMULATOR_INCORE_H
#include "helper.h"

union CPUID_INFO {
    int array[4];
    struct {
        unsigned int eax, ebx, ecx, edx;
    } reg;
};

struct PerfInfo;
class Incore {
    struct PerfInfo perf[4];


    int start_pmc();
    int stop_pmc();
    int init_all_dram_rds(struct __incore *inc, const pid_t pid, const int cpu);
    int init_cpu_l2stall(struct __incore *inc, const pid_t pid, const int cpu);
    int init_cpu_llcl_hits(struct __incore *inc, const pid_t pid, const int cpu);
    int init_cpu_llcl_miss(struct __incore *inc, const pid_t pid, const int cpu);
    int init_pmc(struct __incore *inc, const pid_t pid, const int cpu);
    void fini_pmc(struct __incore *inc);
    int init_all_pmcs(struct __pmu_info *pmu, const pid_t pid);
    void fini_all_pmcs(struct __pmu_info *pmu);
    int read_cpu_elems(struct __incore *inc, struct __cpu_elem *cpu_elem);
};

void pcm_cpuid(unsigned leaf, CPUID_INFO* info);
bool get_cpu_info(struct CPUInfo *);

#endif // CXL_MEM_SIMULATOR_INCORE_H
