//
// Created by victoryang00 on 1/14/23.
//

#include "incore.h"
#include "helper.h"
extern Helper helper;
void pcm_cpuid(const unsigned leaf, CPUID_INFO *info) {
    __asm__ __volatile__("cpuid"
                         : "=a"(info->reg.eax), "=b"(info->reg.ebx), "=c"(info->reg.ecx), "=d"(info->reg.edx)
                         : "a"(leaf));
}

int Incore::start() {
    int i, r = -1;

    for (i = 0; i < this->perf.size(); i++) {
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

    for (i = 0; i < this->perf.size(); i++) {
        r = this->perf[i]->stop();
        if (r < 0) {
            LOG(ERROR) << fmt::format("perf_stop failed. i:{}\n", i);
            return r;
        }
    }
    return r;
}

ssize_t Incore::read_cpu_elems(struct CPUElem *elem) {
    ssize_t r;
    for (auto const &[idx, value] : this->perf | enumerate) {
        r = value->read_pmu(&elem->cpu[idx]);
        if (r < 0) {
            LOG(ERROR) << fmt::format("read cpu_elems[{}] failed.\n", std::get<0>(helper.perf_conf.cha[idx]));
            return r;
        }
        LOG(DEBUG) << fmt::format("read cpu_elems[{}]:{}\n", std::get<0>(helper.perf_conf.cpu[idx]), elem->cpu[idx]);
    }

    return 0;
}

Incore::Incore(const pid_t pid, const int cpu, struct PerfConfig *perf_config) : perf_config(perf_config) {
    /* reset all pmc values */
    for (int i = 0; i < perf_config->cpu.size(); i++) {
        this->perf[i] = init_incore_perf(pid, cpu, std::get<1>(perf_config->cpu[i]), std::get<2>(perf_config->cpu[i]));
    }
}

bool get_cpu_info(struct CPUInfo *cpu_info) {
    char buffer[1024];
    union {
        char cbuf[16];
        int ibuf[16 / sizeof(int)];
    } buf{};
    CPUID_INFO cpuinfo{};

    pcm_cpuid(0, &cpuinfo);

    memset(buffer, 0, 1024);
    memset(buf.cbuf, 0, 16);
    buf.ibuf[0] = cpuinfo.array[1];
    buf.ibuf[1] = cpuinfo.array[3];
    buf.ibuf[2] = cpuinfo.array[2];

    if (strncmp(buf.cbuf, "GenuineIntel", 12) != 0) {
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