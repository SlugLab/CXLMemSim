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
}; // switch num
class CXLCounter {
    CXLRemoteEvent cxl_mem_load_miss_retired_remote_dram;
    CXLRemoteEvent cxl_mem_load_miss_retired_remote_fwd;
    uint64_t cxl_mem_miss_retired_local_dram;

public:
    explicit CXLCounter(int num);
};

#endif // CXL_MEM_SIMULATOR_CXLCOUNTER_H
