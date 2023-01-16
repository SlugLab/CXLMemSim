//
// Created by victoryang00 on 1/14/23.
//

#ifndef CXL_MEM_SIMULATOR_PERF_H
#define CXL_MEM_SIMULATOR_PERF_H

#include <bpf/bpf.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <tuple>
#include <map>
#include <unistd.h>

class PerfInfo {
public:
    int fd;
    int group_fd;
    int cpu;
    pid_t pid;
    unsigned long flags;
    struct perf_event_attr attr;
    PerfInfo();
    PerfInfo(int group_fd, int cpu, pid_t pid, unsigned long flags, struct perf_event_attr attr);
    PerfInfo(int fd, int group_fd, int cpu, pid_t pid, unsigned long flags, struct perf_event_attr attr);
    ~PerfInfo();
    ssize_t read_pmu(uint64_t *value);
    std::map<unsigned long, std::tuple<unsigned long, unsigned long long>> read_trace_pipe();
    int start();
    int stop();
};

PerfInfo init_incore_perf(const pid_t, const int, uint64_t, uint64_t);
PerfInfo init_incore_bpf_perf(const pid_t, const int);

#endif // CXL_MEM_SIMULATOR_PERF_H
