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
PerfInfo::PerfInfo(int group_fd, int cpu, pid_t pid, unsigned long flags, struct perf_event_attr attr)
    : group_fd(group_fd), cpu(cpu), flags(flags), attr(attr) {
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
    nanosleep(&zero, nullptr);
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

static PerfInfo init_incore_perf(const pid_t pid, const int cpu, uint64_t conf, uint64_t conf1) {
    int r, n_pid, n_cpu, group_fd, flags;
    struct perf_event_attr attr {
        .type = PERF_TYPE_RAW, .size = sizeof(attr), .config = conf, .config1 = conf1, .disabled = 1, .inherit = 1,
    };
    if ((0 <= cpu) && (cpu < Helper::num_of_cpu())) {
        n_pid = -1;
        n_cpu = cpu;
    } else {
        n_pid = pid;
        n_cpu = -1;
    }

    group_fd = -1;
    flags = 0x08;

    PerfInfo perf{group_fd, n_cpu, n_pid, static_cast<unsigned long>(flags), attr};
}
