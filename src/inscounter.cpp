/*
 * CXLMemSim instruction counter
 *
 *  By: Shri Vishakh
 *
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "inscounter.h"
#include "helper.h"
#include <spdlog/spdlog.h>
#include <cstring>

InstructionCounter::InstructionCounter(pid_t pid) : pid(pid), last_count(0), current_count(0) {
    // Configure perf_event_attr for instruction counting
    // Uses generic PERF_COUNT_HW_INSTRUCTIONS - works on Intel, AMD, ARM
    perf_event_attr pe = {
        .type = PERF_TYPE_HARDWARE,
        .size = sizeof(perf_event_attr),
        .config = PERF_COUNT_HW_INSTRUCTIONS,
        .disabled = 1,
        .exclude_kernel = 1,
        .exclude_hv = 1,
    };

    int cpu = -1;  // measure on any cpu
    int group_fd = -1;
    unsigned long flags = 0;

    this->fd = perf_event_open(&pe, pid, cpu, group_fd, flags);
    if (this->fd == -1) {
        SPDLOG_ERROR("InstructionCounter perf_event_open failed: {}", strerror(errno));
        return;
    }

    if (this->start() < 0) {
        SPDLOG_ERROR("InstructionCounter start failed");
        close(this->fd);
        this->fd = -1;
        return;
    }

    // Initialize with first read
    this->last_count = this->read_absolute();
}

uint64_t InstructionCounter::read_delta() {
    if (this->fd < 0) {
        return 0;
    }

    uint64_t count = 0;
    ssize_t ret = ::read(this->fd, &count, sizeof(count));
    if (ret != sizeof(count)) {
        SPDLOG_DEBUG("InstructionCounter read failed: {}", strerror(errno));
        return 0;
    }

    uint64_t delta = count - this->last_count;
    this->last_count = count;
    this->current_count = count;

    return delta;
}

uint64_t InstructionCounter::read_absolute() {
    if (this->fd < 0) {
        return 0;
    }

    uint64_t count = 0;
    ssize_t ret = ::read(this->fd, &count, sizeof(count));
    if (ret != sizeof(count)) {
        SPDLOG_DEBUG("InstructionCounter read_absolute failed: {}", strerror(errno));
        return 0;
    }

    this->current_count = count;
    return count;
}

int InstructionCounter::start() const {
    if (this->fd < 0) {
        return 0;
    }
    if (ioctl(this->fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
        perror("ioctl");
        return -1;
    }
    return 0;
}

int InstructionCounter::stop() const {
    if (this->fd < 0) {
        return 0;
    }
    if (ioctl(this->fd, PERF_EVENT_IOC_DISABLE, 0) < 0) {
        perror("ioctl");
        return -1;
    }
    return 0;
}

InstructionCounter::~InstructionCounter() {
    if (this->fd < 0) {
        return;
    }

    this->stop();

    if (this->fd != -1) {
        close(this->fd);
        this->fd = -1;
    }
}
