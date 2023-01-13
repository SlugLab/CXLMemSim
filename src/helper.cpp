//
// Created by victoryang00 on 1/12/23.
//
#include "helper.h"

const struct ModelContext model_ctx[] = {{CPU_MDL_BDX,
                                          {
                                              "/sys/bus/event_source/devices/uncore_cbox_%u/type",
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
                                              0x01b7,
                                              0x6040007f7,
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
                                          }},
                                         {CPU_MDL_SKX,
                                          {
                                              "/sys/bus/event_source/devices/uncore_cha_%u/type",
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
                                              0x01b7,
                                              0xBC408000,
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
                                          }},
                                         {CPU_MDL_SPR,
                                          {
                                              "/sys/bus/event_source/devices/uncore_cha_%u/type",
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
                                              0x01b7,
                                              0x63FC00491,
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
                                          }},
                                         {CPU_MDL_ADL,
                                          {
                                              "/sys/bus/event_source/devices/uncore_cbox_%u/type",
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
                                              0x012a,
                                              0x3fbfc00001,
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
                                          }},
                                         {CPU_MDL_END, {0}}};

int Helper::num_of_cpu(void) { return 0; }

int Helper::num_of_cbo() {
    return 0;
}
