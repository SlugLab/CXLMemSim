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

CXLEndPoint(int read_bw, int write_bw, int read_lat, int write_lat);
};


#endif //CXL_MEM_SIMULATOR_CXLENDPOINT_H
