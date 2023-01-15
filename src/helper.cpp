//
// Created by victoryang00 on 1/12/23.
//
#include "helper.h"
#include "logging.h"

const struct ModelContext model_ctx[] = {{CPU_MDL_BDX,
                                          {"/sys/bus/event_source/devices/uncore_cbox_%u/type",
                                           /*
                                            * cbo_config:
                                            *    unc_c_llc_victims.m_state
                                            *    umask=0x1,event=0x37
                                            */
                                           0x0137,
                                           /*
                                            * all_dram_rds_config:
                                            *   offcore_response.all_reads.llc_miss.local_dram
                                            *   cpu/umask=0x1,event=0xb7,offcore_rsp=0x40007f7/
                                            */
                                           0x01b7, 0x6040007f7,
                                           /*
                                            * cpu_l2stall_config:
                                            *   cycle_activity.stalls_l2_pending
                                            *   cpu/umask=0x5,cmask=0x5,event=0xa3/
                                            */
                                           0x50005a3,
                                           /*
                                            * cpu_llcl_hits_config:
                                            *   mem_load_uops_l3_hit_retired.xsnp_none
                                            *   cpu/umask=0x8,event=0xd2/
                                            */
                                           0x08d2,
                                           /*
                                            * cpu_llcl_miss_config:
                                            *   mem_load_uops_l3_miss_retired.local_dram
                                            *   cpu/umask=0x1,event=0xd3/
                                            */
                                           0x01d3,
                                           /*
                                            * cpu_bandwidth_read_config:
                                            *   UNC_M_CAS_COUNT.RD * 64
                                            *   cpu/umask=0x03,event=0x04/
                                            */
                                           0x0304,
                                           /*
                                            * cpu_bandwidth_write_config:
                                            *   UNC_M_CAS_COUNT.WR * 64
                                            *   cpu/umask=0x0c,event=0x04/
                                            */
                                           0x0c04}},
                                         {CPU_MDL_SKX,
                                          {"/sys/bus/event_source/devices/uncore_cha_%u/type",
                                           /*
                                            * cbo_config:
                                            *   UNC_C_LLC_VICTIMS
                                            *   umask=0x21,event=37
                                            */
                                           0x2137,
                                           /*
                                            * all_dram_rds_config:
                                            *   OCR.ALL_READS.L3_MISS.SNOOP_NONE
                                            *   cpu/umask=0x1,event=0xb7,offcore_rsp=0xBC408000/
                                            */
                                           0x01b7, 0xBC408000,
                                           /*
                                            * cpu_l2stall_config:
                                            *   cycle_activity.stalls_l2_miss
                                            *   cpu/umask=0x5,cmask=0x5,event=0xa3/
                                            */
                                           0x50005a3,
                                           /*
                                            * cpu_llcl_hits_config:
                                            *   mem_load_l3_hit_retired.xsnp_none
                                            *   cpu/umask=0x8,event=0xd2/
                                            */
                                           0x08d2,
                                           /*
                                            * cpu_llcl_miss_config:
                                            *   mem_load_l3_miss_retired.local_dram
                                            *   cpu/umask=0x1,event=0xd3/
                                            */
                                           0x01d3,
                                           /*
                                            * cpu_bandwidth_read_config:
                                            *   UNC_M_CAS_COUNT.RD * 64
                                            *   cpu/umask=0x03,event=0x04/
                                            */
                                           0x0304,
                                           /*
                                            * cpu_bandwidth_write_config:
                                            *   UNC_M_CAS_COUNT.WR * 64
                                            *   cpu/umask=0x0c,event=0x04/
                                            */
                                           0x0c04}},
                                         {CPU_MDL_SPR,
                                          {"/sys/bus/event_source/devices/uncore_cha_%u/type",
                                           /*
                                            * cbo_config:
                                            *   UNC_C_LLC_VICTIMS => OFFCORE_REQUESTS.L3_MISS_DEMAND_DATA_RD
                                            *   umask=0x10,event=b0
                                            */
                                           0x10b0,
                                           /*
                                            * all_dram_rds_config:
                                            *   OCR.ALL_READS.L3_MISS.SNOOP_NONE => L3_MISS.SNOOP_MISS_OR_NO_FWD
                                            *   cpu/umask=0x1,event=0xb7,offcore_rsp=0x63FC00491/
                                            */
                                           0x01b7, 0x63FC00491,
                                           /*
                                            * cpu_l2stall_config:
                                            *   cycle_activity.stalls_l2_miss
                                            *   cpu/umask=0x5,cmask=0x5,event=0xa3/
                                            */
                                           0x50005a3,
                                           /*
                                            * cpu_llcl_hits_config:
                                            *   mem_load_l3_hit_retired.xsnp_none
                                            *   cpu/umask=0x8,event=0xd2/
                                            */
                                           0x08d2,
                                           /*
                                            * cpu_llcl_miss_config:
                                            *   mem_load_l3_miss_retired.local_dram
                                            *   cpu/umask=0x1,event=0xd3/
                                            */
                                           0x01d3,
                                           /*
                                            * cpu_bandwidth_read_config:
                                            *   UNC_M_CAS_COUNT.RD * 64
                                            *   cpu/umask=0xcf,event=0x05/
                                            */
                                           0xcf05,
                                           /*
                                            * cpu_bandwidth_write_config:
                                            *   UNC_M_CAS_COUNT.WR * 64
                                            *   cpu/umask=0xf0,event=0x05/
                                            */
                                           0xf005}},
                                         {CPU_MDL_ADL,
                                          {"/sys/bus/event_source/devices/uncore_cbox_%u/type",
                                           /*
                                            * cbo_config:
                                            *   UNC_C_LLC_VICTIMS => OFFCORE_REQUESTS.L3_MISS_DEMAND_DATA_RD
                                            *   umask=0x21,event=10
                                            */
                                           0x2110,
                                           /*
                                            * all_dram_rds_config:
                                            *   OCR.ALL_READS.L3_MISS.SNOOP_NONE => OCR.DEMAND_DATA_RD.L3_MISS
                                            *   cpu/umask=0x1,event=0x2A,offcore_rsp=0x3FBFC00001/
                                            */
                                           0x012a, 0x3fbfc00001,
                                           /*
                                            * cpu_l2stall_config:
                                            *   cycle_activity.stalls_l2_miss
                                            *   cpu/umask=0x5,cmask=0x5,event=0xa3/
                                            */
                                           0x50005a3,
                                           /*
                                            * cpu_llcl_hits_config:
                                            *   mem_load_l3_hit_retired.xsnp_none
                                            *   cpu/umask=0x8,event=0xd2/
                                            */
                                           0x08d2,
                                           /*
                                            * cpu_llcl_miss_config:
                                            *   mem_load_l3_miss_retired.local_dram
                                            *   cpu/umask=0x1,event=0xd3/
                                            */
                                           0x01d3,
                                           /*
                                            * cpu_bandwidth_read_config:
                                            *   UNC_M_CAS_COUNT_RD * 64
                                            *   cpu/umask=0x00,event=0x22/
                                            */
                                           0x0022,
                                           /*
                                            * cpu_bandwidth_write_config:
                                            *   UNC_M_CAS_COUNT_WR * 64
                                            *   cpu/umask=0x00,event=0x23/
                                            */
                                           0x0023}},
                                         {CPU_MDL_END, {0}}};

int Helper::num_of_cpu() {
    int ncpu;
    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 0) {
        LOG(ERROR) << "sysconf";
    }
    LOG(DEBUG) << fmt::format("num_of_cpu={}\n", ncpu);
    return ncpu;
}

int Helper::num_of_cbo() {
    int ncbo = 0;
    for (; ncbo < 128; ++ncbo) {
        std::string path = fmt::format("/sys/bus/event_source/devices/uncore_cbox_{}/type", ncbo);
        // LOG(DEBUG) << path;
        if (!std::filesystem::exists(path)) {
            break;
        }
    }
    LOG(DEBUG) << fmt::format("num_of_cbo={}\n", ncbo);
    return ncbo;
}

double Helper::cpu_frequency() {
    int i = 0;
    int cpu = 0;
    double cpu_mhz = 0.0;
    double max_cpu_mhz = 0.0;
    std::ifstream fp("/proc/cpuinfo");

    for (std::string line; cpu != this->cpu - 1; std::getline(fp, line)) {
        // LOG(DEBUG) << fmt::format("line: {}\n", line);
        i = std::sscanf(line.c_str(), "cpu MHz : %lf", &cpu_mhz);
        max_cpu_mhz = i == 1 ? std::max(max_cpu_mhz, cpu_mhz) : max_cpu_mhz;
        std::sscanf(line.c_str(), "processor : %d", &cpu);
    }
    LOG(DEBUG) << fmt::format("cpu MHz: {}\n", cpu_mhz);

    return cpu_mhz;
}
PerfConfig Helper::detect_model(uint32_t model) {
    int i = 0;
    LOG(INFO) << fmt::format("Detecting model...{}\n", model);
    while (model_ctx[i].model != CPU_MDL_END) {
        if (model_ctx[i].model == model) {
            return model_ctx[i].perf_conf;
        }
        i++;
    }
    throw;
}
Helper::Helper() {
    cpu = num_of_cpu();
    LOG(DEBUG) << cpu;
    cbo = num_of_cbo();
    cpu_freq = cpu_frequency();
}
int PMUInfo::start_all_pmcs() {
    /* enable all pmcs to count */
    int i, r;

    for (i = 0; i < helper->num_of_cpu(); i++) {
        r = this->cpus[i].start_pmc();
        if (r < 0) {
            LOG(ERROR) << fmt::format("start_pmc failed. cpu:{}\n", i);
            return r;
        }
    }
    return 0;
}
PMUInfo::PMUInfo(pid_t pid, Helper *helper) : helper(helper) {
    int i, r, n;

    n = helper->num_of_cbo();

    for (i = 0; i < n; i++) {
        this->cbos.emplace_back(Uncore(i));
    }

    // unfreeze counters
    r = this->unfreeze_counters_cbo_all();
    if (r < 0) {
        LOG(DEBUG) << fmt::format("unfreeze_counters_cbo_all failed.\n");
        throw;
    }

    n = helper->num_of_cpu();

    for (i = 0; i < n; i++) {
         this->cpus.emplace_back(Incore(pid,i));
    }

    r = this->start_all_pmcs();
    if (r < 0) {
        LOG(ERROR) << fmt::format("start_all_pmcs failed\n");
    }
}
int PMUInfo::stop_all_pmcs() { return 0; }
int PMUInfo::init_all_pmcs(const pid_t pid) { return 0; }
void PMUInfo::fini_all_pmcs() {}
int PMUInfo::unfreeze_counters_cbo_all() {
    int i, r;

    for (i = 0; i < helper->num_of_cbo(); i++) {
        r = this->cbos[i].perf.start();
        if (r < 0) {
            LOG(DEBUG) << fmt::format("perf_start failed. cbo:{}\n", i);
            return r;
        }
    }
    return 0;
}
int PMUInfo::freeze_counters_cbo_all() { return 0; }
