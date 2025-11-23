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
#include "bpftimeruntime.h"
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
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

BpfTimeRuntime::BpfTimeRuntime(pid_t tid, std::string program_location)
    : tid(tid), updater(new BPFUpdater<uint64_t, uint64_t>(10)) {
    bpftime_initialize_global_shm(bpftime::shm_open_type::SHM_REMOVE_AND_CREATE);
    SPDLOG_INFO("GLOBAL memory initialized ");
    // load json program to shm
    bpftime_import_global_shm_from_json(program_location.c_str());
    SPDLOG_INFO("Program loaded to shm");
}

BpfTimeRuntime::~BpfTimeRuntime() {}
int BpfTimeRuntime::read(CXLController *controller, BPFTimeRuntimeElem *elem) {
    mem_stats stats;
    proc_info proc_info1;
    proc_info thread_info1;

    // Map FDs from cxlmemsim.json:
    // FD 6: stats_map (key_size: 4)
    // FD 7: allocs_map (key_size: 8) - skip, different key size
    // FD 8: process_map (key_size: 4) - updated by clone/fork
    // FD 9: thread_map (key_size: 4)
    // FD 10: locks (key_size: 4)

    // Only iterate maps with 4-byte keys that we care about
    int maps_to_read[] = {6, 8, 9};

    for (int i : maps_to_read) {
        uint32_t key = 0;
        uint32_t prev_key = 0;
        bool first = true;

        while (true) {
            // First call: pass NULL to get first key
            // Subsequent calls: pass pointer to previous key
            int ret = bpftime_map_get_next_key(i, first ? nullptr : &prev_key, &key);
            if (ret != 0) {
                SPDLOG_DEBUG("No more keys for map {}", i);
                break;
            }
            first = false;

            void *item2 = (void *)bpftime_map_lookup_elem(i, &key);
            SPDLOG_DEBUG("Map {} key: {} thread_id:{}", i, key,
                         std::this_thread::get_id());

            if (i == 6 && item2 != nullptr) {
                // stats_map
                stats = *((mem_stats *)item2);
                controller->set_stats(stats);
                elem->total++;
            }
            if (i == 8 && item2 != nullptr) {
                // process_map - updated by clone/fork probes
                proc_info1 = *((proc_info *)item2);
                controller->set_thread_info(proc_info1);
                elem->total++;
            }
            if (i == 9 && item2 != nullptr) {
                // thread_map
                thread_info1 = *((proc_info *)item2);
                controller->set_thread_info(thread_info1);
                elem->total++;
            }
            prev_key = key;
        }
    }
    return 0;
}
