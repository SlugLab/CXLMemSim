//
// Created by victoryang00 on 1/12/23.
//

#ifndef CXL_MEM_SIMULATOR_CXLCOUNTER_H
#define CXL_MEM_SIMULATOR_CXLCOUNTER_H

#include <cstdint>
#include <vector>

class CXLSwitchEvent {
    uint64_t dram;
    uint64_t fwd;
    uint64_t queue_conflict;
};
union CXLRemoteEvent {
    uint64_t no_switch_event;
    CXLSwitchEvent switch_event;
};
class CXLCPUEvent {
    std::vector<CXLRemoteEvent> cxl_mem_load_uops_l3_miss_retired_remote_dram;
    std::vector<CXLRemoteEvent> cxl_mem_load_uops_l3_miss_retired_remote_fwd;
    uint64_t cxl_mem_load_uops_l3_miss_retired_local_dram;
    explicit CXLCPUEvent(int num);
};

#endif // CXL_MEM_SIMULATOR_CXLCOUNTER_H
