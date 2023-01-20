// Created by victoryang00 on 1/14/23.
//

#ifndef CXL_MEM_SIMULATOR_INCORE_H
#define CXL_MEM_SIMULATOR_INCORE_H
#include "helper.h"
#include "perf.h"
#include <sys/types.h>
union CPUID_INFO {
    int array[4];
    struct {
        unsigned int eax, ebx, ecx, edx;
    } reg;
};

class Incore {
public:
    PerfInfo *perf[7];
    struct PerfConfig *perf_config;
    Incore(const pid_t pid, const int cpu, struct PerfConfig *perf_config);
    ~Incore() = default;
    int start();
    int stop();
    void init_all_dram_rds(const pid_t pid, const int cpu);
    void init_cpu_l2stall(const pid_t pid, const int cpu);
    void init_cpu_llcl_hits(const pid_t pid, const int cpu);
    void init_cpu_llcl_miss(const pid_t pid, const int cpu);
    void init_cpu_mem_read(const pid_t pid, const int cpu);
    void init_cpu_mem_write(const pid_t pid, const int cpu);
    void init_cpu_ebpf(const pid_t pid, const int cpu);

    int read_cpu_elems(struct CPUElem *cpu_elem);
};

void pcm_cpuid(unsigned leaf, CPUID_INFO *info);
bool get_cpu_info(struct CPUInfo *);

#endif // CXL_MEM_SIMULATOR_INCORE_H
