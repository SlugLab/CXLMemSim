/*
 * CXLMemSim bpftime runtime
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */
#include "bpftime_config.hpp"
#include "bpftime_logger.hpp"
#include "bpftime_shm.hpp"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>
#include "bpftimeruntime.h"

BpfTimeRuntime::BpfTimeRuntime(pid_t tid, std::string program_location) : tid(tid) {
    bpftime_initialize_global_shm(bpftime::shm_open_type::SHM_REMOVE_AND_CREATE);
    SPDLOG_INFO("GLOBAL memory initialized ");
    // load json program to shm
    bpftime_import_global_shm_from_json(program_location.c_str());
    SPDLOG_INFO("Program loaded to shm");
}

BpfTimeRuntime::~BpfTimeRuntime() { bpftime_remove_global_shm(); }

int BpfTimeRuntime::read(CXLController *controller, BPFTimeRuntimeElem *elem) {
    mem_stats stats;
    proc_info proc_info1;
    proc_info thread_info1;
    for (int i = 6; i < 11; i++) {
        int key = 0;
        int key1 = 0;
        bpftime_map_get_next_key(i, &key1, &key); // process map
        auto item2 = bpftime_map_lookup_elem(i, &key); // allocs map
        SPDLOG_DEBUG("Process map key: {} {} {}", key1, key, tid);
        if (i == 6 && item2 != nullptr) {
            stats = *((mem_stats *)item2);
            controller->set_stats(stats);
            elem->total++;
        }
        if (i == 9 && item2 != nullptr) {
            proc_info1 = *((proc_info *)item2);
            controller->set_process_info(proc_info1);
            elem->total++;
        }
        if (i == 10 && item2 != nullptr) {
            thread_info1 = *((proc_info *)item2);
            controller->set_thread_info(thread_info1);
            elem->total++;
        }
    }
    return 0;
}
