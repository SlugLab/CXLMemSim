//
// Created by victoryang00 on 1/11/23.
//

#ifndef SLUGALLOCATOR_MONITOR_H
#define SLUGALLOCATOR_MONITOR_H

#include "helper.h"
#include "pebs.h"
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <time.h>

class Monitor {
    void disable_mon(const uint32_t, );
    int enable_mon(const uint32_t, const uint32_t, bool, uint64_t, const int32_t, );
    int terminate_mon(const uint32_t, const uint32_t, const int32_t, );
    int set_region_info_mon(, const int, struct __region_info *);
    void initMon(const int, cpu_set_t *, *, const int);
    void freeMon(const int, *);
    void stop_all_mons(const uint32_t, );
    void run_all_mons(const uint32_t, );
    void stop_mon();
    void run_mon();
    bool check_continue_mon(const uint32_t, const struct timespec);
    void clear_mon_time(struct timespec *);
    bool check_all_mons_terminated(const uint32_t, );
};

#endif // SLUGALLOCATOR_MONITOR_H
