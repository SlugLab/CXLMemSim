//
// Created by victoryang00 on 1/14/23.
//

#ifndef CXLMEMSIM_PERF_H
#define CXLMEMSIM_PERF_H

#include <bpf/bpf.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <thread>
#include <tuple>
#include <unistd.h>

class PerfInfo {
public:
    int fd;
    int group_fd;
    int cpu;
    pid_t pid;
    unsigned long flags;
    struct perf_event_attr attr;
    PerfInfo() = default;
    PerfInfo(int group_fd, int cpu, pid_t pid, unsigned long flags, struct perf_event_attr attr);
    ~PerfInfo();
    ssize_t read_pmu(uint64_t *value);
    int start();
    int stop();
};

PerfInfo *init_incore_perf(const pid_t pid, const int cpu, uint64_t conf, uint64_t conf1);
PerfInfo *init_uncore_perf(const pid_t pid, const int cpu, uint64_t conf, uint64_t conf1, int value);
#endif // CXLMEMSIM_PERF_H
