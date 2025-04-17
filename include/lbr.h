/*
 * CXLMemSim lbr
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#ifndef CXLMEMSIM_LBR_H
#define CXLMEMSIM_LBR_H

// 2 bits
#define LBR_DATA_MASK 0x0000000000000003
#define LBR_DATA_SHIFT 0
// 8 bits
#define LBR_INS_MASK 0xfffffff0000
#define LBR_INS_SHIFT 16

#include "cxlcontroller.h"
#include "helper.h"
#include <linux/perf_event.h>
#include <sys/mman.h>
class CXLController; // Forward declaration

struct lbr {
    uint64_t from;
    uint64_t to;
    uint64_t flags;
};
struct cntr {
    uint64_t counters;
};
struct lbr_sample {
    perf_event_header header;
    uint32_t pid;
    uint32_t tid;
    uint64_t timestamp;
    uint32_t cpu;
    uint64_t nr2;
    lbr lbrs[32];
    cntr counters[32];
};

class LBR {
public:
    int fd;
    int pid;
    uint64_t sample_period;
    uint32_t seq{};
    size_t rdlen{};
    size_t mplen{};
    bool use_pe2 = false;
    perf_event_mmap_page *mp;
    explicit LBR(pid_t, uint64_t);
    ~LBR();
    int read(CXLController *, LBRElem *);
    int start() const;
    int stop() const;
};

#endif // CXLMEMSIM_LBR_H
