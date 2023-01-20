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
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <thread>
#include <tuple>
#include <unistd.h>

class ThreadSafeMap {
public:
    ThreadSafeMap() = default;

    // Multiple threads/readers can read the Map's value at the same time.
    std::map<unsigned long, std::tuple<unsigned long, unsigned long long int>> get() const {
        std::shared_lock lock(mutex_);
        return res;
    }

    // Only one thread/writer can increment/write the Map's value.
    void insert(unsigned long address, unsigned long size, unsigned long long time) {
        std::unique_lock lock(mutex_);
        res[address] = std::make_tuple(size, time);
    }

    // Only one thread/writer can reset/write the Map's value.
    void reset() {
        std::unique_lock lock(mutex_);
        res.clear();
    }

private:
    mutable std::shared_mutex mutex_;
    std::map<unsigned long, std::tuple<unsigned long, unsigned long long>> res;
};

class PerfInfo {
public:
    int fd;
    int group_fd;
    int cpu;
    pid_t pid;
    unsigned long flags;
    struct perf_event_attr attr;
    ThreadSafeMap *map;
//    PerfInfo();
    PerfInfo(int group_fd, int cpu, pid_t pid, unsigned long flags, struct perf_event_attr attr);
    PerfInfo(int fd, int group_fd, int cpu, pid_t pid, unsigned long flags, struct perf_event_attr attr);
    ~PerfInfo();
    ssize_t read_pmu(uint64_t *value);
    std::map<uint64_t, uint64_t> read_trace_pipe();
    int start();
    int stop();
};

PerfInfo *init_incore_perf(const pid_t pid, const int cpu, uint64_t conf, uint64_t conf1);
PerfInfo *init_incore_bpf_perf(const pid_t pid, const int cpu);
void write_trace_to_map(ThreadSafeMap *map);
#endif // CXL_MEM_SIMULATOR_PERF_H
