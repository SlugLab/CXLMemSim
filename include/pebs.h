//
// Created by victoryang00 on 1/13/23.
//

#ifndef CXLMEMSIM_PEBS_H
#define CXLMEMSIM_PEBS_H

#include "cxlcontroller.h"
#include "helper.h"
#include <asm/unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <x86intrin.h>

long perf_event_open(struct perf_event_attr *event_attr, pid_t pid, int cpu, int group_fd, unsigned long flags);
class PEBS {
public:
    int fd;
    int pid;
    uint64_t sample_period;
    uint32_t seq{};
    size_t rdlen{};
    size_t mplen{};
    struct perf_event_mmap_page *mp;
    PEBS(pid_t, uint64_t);
    ~PEBS();
    int read(CXLController *, struct PEBSElem *);
    int start();
    int stop();
};

#endif // CXLMEMSIM_PEBS_H
