/*
 * CXLMemSim rob
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#ifndef CXLMEMSIM_ROB_H
#define CXLMEMSIM_ROB_H

#include "cxlcontroller.h"
#include <deque>
#include <string>
#include <vector>

// Structure holding the minimal info we want.
struct InstructionGroup {
    long long address; // optional
    long long cycleCount = 0;
    long long fetchTimestamp = 0;
    long long retireTimestamp = 0;
    std::string instruction; // combined opcode and text
};

class Rob {
public:
    explicit Rob(CXLController *controller, size_t size = 256, uint64_t cycle = 1687)
        : controller_(controller), maxSize_(size), currentCycle_(cycle) {}

    CXLController *controller_;
    const size_t maxSize_;
    std::deque<InstructionGroup> queue_; // ROB
    int64_t stallCount_ = 0;
    int64_t stallEventCount_ = 0;
    int64_t cur_latency = 0;
    int64_t last_latency = 0;
    int64_t totalLatency_ = 0;
    int64_t currentCycle_ = 0;
    int counter = 0;
    bool issue(const InstructionGroup &ins);
    bool canRetire(const InstructionGroup &ins);
    bool tryAlternativeRetire();
    void tick();
    // Trace generation
    void saveInstructionTrace(const std::vector<InstructionGroup> &instructions, const std::string &outputFile,
                              bool append = false);

    int64_t getStallCount() const { return stallCount_; }
    int64_t getStallEventCount() const { return stallEventCount_; }
    int64_t getCurrentCycle() const { return currentCycle_; }
    double getAverageLatency() const { return queue_.empty() ? 0 : static_cast<double>(totalLatency_) / queue_.size(); }

    void resetCounters() {
        stallCount_ = 0;
        stallEventCount_ = 0;
        totalLatency_ = 0;
    }
};

#endif // CXLMEMSIM_ROB_H
