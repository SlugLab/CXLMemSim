//
// Created by victoryang00 on 1/12/23.
//

#ifndef CXL_MEM_SIMULATOR_UNCORE_H
#define CXL_MEM_SIMULATOR_UNCORE_H
#include "helper.h"
#include "perf.h"
#include <cstdint>
class Uncore {
public:
    uint32_t unc_idx;
    PerfInfo perf;
    Uncore(const uint32_t unc_idx);
    ~Uncore();

    int init_all_cbos();
    void fini_all_cbos();
    int read_cbo_elems(struct CBOElem *elem);
};

#endif // CXL_MEM_SIMULATOR_UNCORE_H
