//
// Created by victoryang00 on 1/14/23.
//

#ifndef CXL_MEM_SIMULATOR_CXLREGION_H
#define CXL_MEM_SIMULATOR_CXLREGION_H

#include <cstdint>

class CXLRegion {
public:
    uint64_t addr;
    uint64_t size;
};

#endif // CXL_MEM_SIMULATOR_CXLREGION_H
