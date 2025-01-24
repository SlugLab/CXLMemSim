/*
 * CXLMemSim pebs
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */


#ifndef CXLMEMSIM_PEBS_H
#define CXLMEMSIM_PEBS_H

#include "helper.h"
#include <sys/mman.h>
#include <cstdint>
#include <cxlcontroller.h>
#include <sys/types.h>

class PEBS {
public:
    int fd;
    int pid;
    uint64_t sample_period;
    uint32_t seq{};
    size_t rdlen{};
    size_t mplen{};
    perf_event_mmap_page *mp;
    PEBS(pid_t, uint64_t);
    ~PEBS();
    int read(CXLController *, PEBSElem *);
    int start() const;
    int stop() const;
};

#endif // CXLMEMSIM_PEBS_H
