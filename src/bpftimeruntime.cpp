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
#include <thread>
#include "bpftimeruntime.h"

BpfTimeRuntime::BpfTimeRuntime(pid_t tid, std::string program_location)
    : tid(tid), updater(new BPFUpdater<uint64_t, uint64_t>(10)) {
    bpftime_initialize_global_shm(bpftime::shm_open_type::SHM_OPEN_ONLY);
    SPDLOG_INFO("GLOBAL memory initialized ");
}

BpfTimeRuntime::~BpfTimeRuntime() {}
int BpfTimeRuntime::read(CXLController *controller, BPFTimeRuntimeElem *elem) {
    mem_stats stats;
    proc_info proc_info1;
    proc_info thread_info1;

    for (int i = 6; i < 11; i++) {
        uint64_t key = 0; // 改为8字节
        uint64_t key1 = 0; // 改为8字节
        void *item2 = (void *)1;
        while (item2) {
            int ret = bpftime_map_get_next_key(i, &key1, &key); // 获取key
            if (ret != 0) {
                SPDLOG_DEBUG("Failed to get next key for map {}", i);
                break;
            }

            item2 = (void *)bpftime_map_lookup_elem(i, &key);
            SPDLOG_DEBUG("Process map key: {} {} thread_id:{}", key1, key,
                         std::this_thread::get_id()); // 使用std::this_thread获取当前线程ID

            if (i == 8 && item2 != nullptr) {
                stats = *((mem_stats *)item2);
                controller->set_stats(stats);
                elem->total++;
            }
            if (i == 6 && item2 != nullptr) {
                proc_info1 = *((proc_info *)item2);
                controller->set_process_info(proc_info1);
                elem->total++;
            }
            if (i == 7 && item2 != nullptr) {
                thread_info1 = *((proc_info *)item2);
                controller->set_thread_info(thread_info1);
                elem->total++;
            }
            key1 = key; // 更新key1为当前key
        }
    }
    return 0;
}
