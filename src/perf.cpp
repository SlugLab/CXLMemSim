/*
 * CXLMemSim perf
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "perf.h"
#include "pebs.h"

PerfInfo::PerfInfo(int group_fd, int cpu, pid_t pid, unsigned long flags, perf_event_attr attr)
    : group_fd(group_fd), cpu(cpu), pid(pid), flags(flags), attr(attr) {
    this->fd = perf_event_open(&this->attr, this->pid, this->cpu, this->group_fd, this->flags);
    if (this->fd == -1) {
        SPDLOG_ERROR("perf_event_open");
        throw;
    }
    ioctl(this->fd, PERF_EVENT_IOC_RESET, 0);
}
PerfInfo::~PerfInfo() {
    if (this->fd != -1) {
        close(this->fd);
        this->fd = -1;
    }
}
/*
 * Workaround:
 *   The expected value cannot be obtained when reading continuously.
 *   This can be avoided by executing nanosleep with 0.
 */
ssize_t PerfInfo::read_pmu(uint64_t *value) {
    ssize_t r = read(this->fd, value, sizeof(*value));
    if (r < 0) {
        SPDLOG_ERROR("read failed\n");
    }
    return r;
}
int PerfInfo::start() {
    if (ioctl(this->fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
        SPDLOG_ERROR("ioctl failed\n");
        return -1;
    }
    return 0;
}
int PerfInfo::stop() {
    if (ioctl(this->fd, PERF_EVENT_IOC_DISABLE, 0) < 0) {
        SPDLOG_ERROR("ioctl failed\n");
        return -1;
    }
    return 0;
}

PerfInfo *init_incore_perf(const pid_t pid, const int cpu, uint64_t conf, uint64_t conf1) {
    int n_pid, n_cpu, group_fd, flags;
    struct perf_event_attr attr {
        .type = PERF_TYPE_RAW, .size = sizeof(attr), .config = conf, .disabled = 1, .inherit = 1, .config1 = conf1,
        .clockid = 0
    };
    n_pid = -1;
    n_cpu = cpu;

    group_fd = -1;
    flags = 0x08;

    return new PerfInfo{group_fd, n_cpu, n_pid, static_cast<unsigned long>(flags), attr};
}

PerfInfo *init_uncore_perf(const pid_t pid, const int cpu, uint64_t conf, uint64_t conf1, int value) {
    int group_fd = -1;
    auto attr = perf_event_attr{
        .type = (uint32_t)value,
        .size = sizeof(struct perf_event_attr),
        .config = conf,
        .disabled = 1,
        .inherit = 1,
        .enable_on_exec = 1,
        .config1 = conf1,
    };

    return new PerfInfo{group_fd, cpu, pid, 0, attr};
}