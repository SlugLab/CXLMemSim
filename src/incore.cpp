//
// Created by victoryang00 on 1/14/23.
//

#include "incore.h"

void pcm_cpuid(const unsigned leaf, CPUID_INFO *info) {
    __asm__ __volatile__("cpuid"
                         : "=a"(info->reg.eax), "=b"(info->reg.ebx), "=c"(info->reg.ecx), "=d"(info->reg.edx)
                         : "a"(leaf));
}

int Incore::start_pmc() { return 0; }
int Incore::stop_pmc() { return 0; }
int Incore::init_all_dram_rds(const pid_t pid, const int cpu) { return 0; }
int Incore::init_cpu_l2stall(const pid_t pid, const int cpu) { return 0; }
int Incore::init_cpu_llcl_hits(const pid_t pid, const int cpu) { return 0; }
int Incore::init_cpu_llcl_miss(const pid_t pid, const int cpu) { return 0; }

int Incore::read_cpu_elems(struct __incore *inc, struct __cpu_elem *cpu_elem) { return 0; }
Incore::Incore(const pid_t pid, const int cpu) {

}
Incore::~Incore() {}
