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
    int id = -1;
    CXLEndPoint *lChild = nullptr, *rChild = nullptr, *father = nullptr;
    float lDist = 0, rDist = 0, fDist = 0;
    bool cFlag;
    CXLEndPoint(int read_bw, int write_bw, int read_lat, int write_lat);
    double calculate_latency(double weight,struct Elem*elem);//traverse the tree to calculate the latency
    double calculate_bandwidth(double weight,struct Elem*elem );
};

#endif // CXL_MEM_SIMULATOR_CXLENDPOINT_H
