//
// Created by victoryang00 on 1/14/23.
//

#include "perf.h"
#include "pebs.h"

PerfInfo::PerfInfo() {
    this->fd = perf_event_open(&this->attr, this->pid, this->cpu, this->group_fd, this->flags);
    if (this->fd == -1) {
        LOG(ERROR) << "perf_event_open";
        throw;
    }
    ioctl(this->fd, PERF_EVENT_IOC_RESET, 0);
}
PerfInfo::~PerfInfo() {}
/*
 * Workaround:
 *   The expected value cannot be obtained when reading continuously.
 *   This can be avoided by executing nanosleep with 0.
 */
ssize_t PerfInfo::read_pmu(uint64_t *value) {
    struct timespec zero = {0};
    nanosleep(&zero, NULL);
    ssize_t r = read(this->fd, value, sizeof(*value));
    if (r < 0) {
        LOG(ERROR) << "read";
    }
    return r;
}
int PerfInfo::start() {
    if (ioctl(this->fd, PERF_EVENT_IOC_ENABLE, 0) < 0) {
        LOG(ERROR) << "ioctl";
        return -1;
    }
    return 0;
}
int PerfInfo::stop() {
    if (ioctl(this->fd, PERF_EVENT_IOC_DISABLE, 0) < 0) {
        LOG(ERROR) << "ioctl";
        return -1;
    }
    return 0;
}
