//
// Created by victoryang00 on 1/13/23.
//

#ifndef CXL_MEM_SIMULATOR_CXLENDPOINT_H
#define CXL_MEM_SIMULATOR_CXLENDPOINT_H

#include "helper.h"
#include <unordered_map>

class CXLEndpoint {
    EmuCXLBandwidth band;
    EmuCXLLatency lat;

public:
    int idx;
    CXLEndpoint(int idx, EmuCXLBandwidth band, EmuCXLLatency lat);
    // Both cacheline mode and page table mode has a start_addr
    std::unordered_map<uint64_t, uint64_t> mem_access_table;
    void insert(uint64_t timestamp, uint64_t start_addr);
    int calculate_latency();
    int calculate_bandwidth();
    void migrate();
};

#endif // CXL_MEM_SIMULATOR_CXLENDPOINT_H
