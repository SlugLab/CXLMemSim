//
// Created by victoryang00 on 1/14/23.
//

#include "incore.h"
#include "helper.h"

void pcm_cpuid(const unsigned leaf, CPUID_INFO *info) {
    __asm__ __volatile__("cpuid"
                         : "=a"(info->reg.eax), "=b"(info->reg.ebx), "=c"(info->reg.ecx), "=d"(info->reg.edx)
                         : "a"(leaf));
}

int Incore::start() {
    int i, r = -1;

    for (i = 0; i < 7; i++) {
        r = this->perf[i]->start();
        if (r < 0) {
            LOG(ERROR) << fmt::format("perf_start failed. i:{}\n", i);
            return r;
        }
    }
    return r;
}
int Incore::stop() {
    int i, r = -1;

    for (i = 0; i < 7; i++) {
        r = this->perf[i]->stop();
        if (r < 0) {
            LOG(ERROR) << fmt::format("perf_stop failed. i:{}\n", i);
            return r;
        }
    }
    return r;
}
void Incore::init_all_dram_rds(const pid_t pid, const int cpu) {
    this->perf[0] = init_incore_perf(pid, cpu, perf_config->all_dram_rds_config, perf_config->all_dram_rds_config1);
}
void Incore::init_cpu_l2stall(const pid_t pid, const int cpu) {
    this->perf[1] = init_incore_perf(pid, cpu, perf_config->cpu_l2stall_config, 0);
}
void Incore::init_cpu_llcl_hits(const pid_t pid, const int cpu) {
    this->perf[2] = init_incore_perf(pid, cpu, perf_config->cpu_llcl_hits_config, 0);
}
void Incore::init_cpu_llcl_miss(const pid_t pid, const int cpu) {
    this->perf[3] = init_incore_perf(pid, cpu, perf_config->cpu_llcl_miss_config, 0);
}
void Incore::init_cpu_mem_read(const pid_t pid, const int cpu) {
    this->perf[4] = init_incore_perf(pid, cpu, perf_config->cpu_bandwidth_read_config, 0);
}
void Incore::init_cpu_mem_write(const pid_t pid, const int cpu) {
    this->perf[5] = init_incore_perf(pid, cpu, perf_config->cpu_bandwidth_write_config, 0);
}
void Incore::init_cpu_ebpf(const pid_t pid, const int cpu) { this->perf[6] = init_incore_bpf_perf(pid, cpu); }
int Incore::read_cpu_elems(struct CPUElem *elem) {
    ssize_t r;

    r = this->perf[0]->read_pmu(&elem->all_dram_rds);
    if (r < 0) {
        LOG(ERROR) << fmt::format("read all_dram_rds failed.\n");
        return r;
    }
    LOG(DEBUG) << fmt::format("read all_dram_rds:{}\n", elem->all_dram_rds);

    r = this->perf[1]->read_pmu(&elem->cpu_l2stall_t);
    if (r < 0) {
        LOG(ERROR) << fmt::format("read cpu_l2stall_t failled.\n");
        return r;
    }
    LOG(DEBUG) << fmt::format("read cpu_l2stall_t:{}\n", elem->cpu_l2stall_t);

    r = this->perf[2]->read_pmu(&elem->cpu_llcl_hits);
    if (r < 0) {
        LOG(ERROR) << fmt::format("read cpu_llcl_hits failed.\n");
        return r;
    }
    LOG(DEBUG) << fmt::format("read cpu_llcl_hits:{}\n", elem->cpu_llcl_hits);

    r = this->perf[3]->read_pmu(&elem->cpu_llcl_miss);
    if (r < 0) {
        LOG(ERROR) << fmt::format("read cpu_llcl_miss failed.\n");
        return r;
    }
    LOG(DEBUG) << fmt::format("read cpu_llcl_miss:{}\n", elem->cpu_llcl_miss);

    r = this->perf[4]->read_pmu(&elem->cpu_bandwidth_read);
    if (r < 0) {
        LOG(ERROR) << fmt::format("read cpu_bandwidth_read failed.\n");
        return r;
    }
    LOG(DEBUG) << fmt::format("read cpu_bandwidth_read:{}\n", elem->cpu_bandwidth_read);
    r = this->perf[5]->read_pmu(&elem->cpu_bandwidth_write);
    if (r < 0) {
        LOG(ERROR) << fmt::format("read cpu_bandwidth_write failed.\n");
        return r;
    }
    LOG(DEBUG) << fmt::format("read cpu_bandwidth_write:{}\n", elem->cpu_bandwidth_write);
//    elem->cpu_munmap_address_length = this->perf[6]->read_trace_pipe();
//    LOG(DEBUG) << fmt::format("read munmap result with size:{}\n", elem->cpu_munmap_address_length.size());
}
Incore::Incore(const pid_t pid, const int cpu, struct PerfConfig *perf_config) : perf_config(perf_config) {
    /* reset all pmc values */
    this->init_all_dram_rds(pid, cpu);
    this->init_cpu_l2stall(pid, cpu);
    this->init_cpu_llcl_hits(pid, cpu);
    this->init_cpu_llcl_miss(pid, cpu);
    this->init_cpu_mem_read(pid, cpu);
    this->init_cpu_mem_write(pid, cpu);
//    this->init_cpu_ebpf(pid, cpu);
}
bool get_cpu_info(struct CPUInfo *cpu_info) {
    char buffer[1024];
    union {
        char cbuf[16];
        int ibuf[16 / sizeof(int)];
    } buf;
    CPUID_INFO cpuinfo;

    pcm_cpuid(0, &cpuinfo);

    memset(buffer, 0, 1024);
    memset(buf.cbuf, 0, 16);
    buf.ibuf[0] = cpuinfo.array[1];
    buf.ibuf[1] = cpuinfo.array[3];
    buf.ibuf[2] = cpuinfo.array[2];

    if (strncmp(buf.cbuf, "GenuineIntel", 4 * 3) != 0) {
        LOG(ERROR) << fmt::format("We only Support Intel CPU\n");
        return false;
    }

    cpu_info->max_cpuid = cpuinfo.array[0];

    pcm_cpuid(1, &cpuinfo);
    cpu_info->cpu_family = (((cpuinfo.array[0]) >> 8) & 0xf) | ((cpuinfo.array[0] & 0xf00000) >> 16);
    cpu_info->cpu_model = (((cpuinfo.array[0]) & 0xf0) >> 4) | ((cpuinfo.array[0] & 0xf0000) >> 12);
    cpu_info->cpu_stepping = cpuinfo.array[0] & 0x0f;

    if (cpuinfo.reg.ecx & (1UL << 31UL)) {
        LOG(ERROR) << fmt::format("Detected a hypervisor/virtualization technology. "
                                  "Some metrics might not be available due to configuration "
                                  "or availability of virtual hardware features.\n");
    }

    if (cpu_info->cpu_family != 6) {
        LOG(ERROR) << fmt::format("It doesn't support this CPU. CPU Family: %u\n", cpu_info->cpu_family);
        return false;
    }

    LOG(DEBUG) << fmt::format("MAX_CPUID={}, CPUFAMILY={}, CPUMODEL={}, CPUSTEPPING={}\n", cpu_info->max_cpuid,
                              cpu_info->cpu_family, cpu_info->cpu_model, cpu_info->cpu_stepping);

    return true;
}