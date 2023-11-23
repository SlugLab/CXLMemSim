// Created by victoryang00 on 1/14/23.
//

#ifndef CXLMEMSIM_INCORE_H
#define CXLMEMSIM_INCORE_H
#include "helper.h"
#include "perf.h"
#include <array>
#include <cstdint>

class CXLController; // TODO: need to be shm gotten
union CPUID_INFO {
    int array[4];
    struct {
        unsigned int eax, ebx, ecx, edx;
    } reg;
};
/** This is a per cha metrics*/
class Incore {
public:
    std::array<PerfInfo *, 4> perf{nullptr}; // should only be 4 counters
    struct PerfConfig *perf_config;
    Incore(pid_t pid, int cpu, struct PerfConfig *perf_config);
    ~Incore() = default;
    int start();
    int stop();

    ssize_t read_cpu_elems(struct CPUElem *cpu_elem);
};

void pcm_cpuid(unsigned leaf, CPUID_INFO *info);
bool get_cpu_info(struct CPUInfo *);

#endif // CXLMEMSIM_INCORE_H
