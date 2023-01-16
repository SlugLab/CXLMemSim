//
// Created by victoryang00 on 1/13/23.
//

#ifndef CXL_MEM_SIMULATOR_CXLENDPOINT_H
#define CXL_MEM_SIMULATOR_CXLENDPOINT_H
#include "helper.h"
class CXLEndPoint {
public:
    EmuCXLBandwidth bandwidth;
    EmuCXLLatency latency;
    std::map<uint64_t,  uint64_t> occupation;
    std::vector<uint64_t> lazy_add;
    std::vector<uint64_t> lazy_remove;

    int id = -1;
    CXLEndPoint *lChild = nullptr, *rChild = nullptr, *father = nullptr;
    float lDist = 0, rDist = 0, fDist = 0;
    bool cFlag;
    CXLEndPoint(int read_bw, int write_bw, int read_lat, int write_lat,int id);
    double calculate_latency(double weight,struct Elem*elem);//traverse the tree to calculate the latency
    double calculate_bandwidth(double weight,struct Elem*elem );
    void delete_entry(uint64_t addr);
};

#endif // CXL_MEM_SIMULATOR_CXLENDPOINT_H
