/*
 * CXLMemSim instruction counter
 *
 *  By: Shri Vishakh
 *
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#ifndef CXLMEMSIM_INSCOUNTER_H
#define CXLMEMSIM_INSCOUNTER_H

#include <cstdint>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <unistd.h>

class InstructionCounter {
public:
    int fd;
    pid_t pid;
    uint64_t last_count;
    uint64_t current_count;

    explicit InstructionCounter(pid_t pid);
    ~InstructionCounter();

    // Read current instruction count and return delta since last read
    uint64_t read_delta();

    // Get absolute current count
    uint64_t read_absolute();

    int start() const;
    int stop() const;
};

#endif // CXLMEMSIM_INSCOUNTER_H
