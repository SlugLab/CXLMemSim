/*
 * CXLMemSim uncore
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#ifndef CXLMEMSIM_UNCORE_H
#define CXLMEMSIM_UNCORE_H
#include "helper.h"
#include "perf.h"
#include <array>
#include <cstdint>

struct PerfConfig;
class Uncore {
public:
    uint32_t unc_idx{};
    int fd{};
    std::array<PerfInfo *, 4> perf{nullptr, nullptr, nullptr, nullptr};
    Uncore(uint32_t unc_idx, PerfConfig *perf_config);

    ~Uncore() = default;

    int read_cha_elems(struct CHAElem *elem);
};

#endif // CXLMEMSIM_UNCORE_H
