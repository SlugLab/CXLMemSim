/*
 * CXLMemSim bpftime runtime
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */
#include "bpftime_config.hpp"
#include "bpftime_logger.hpp"
#include "bpftime_shm.hpp"
#include "bpftime_shm_internal.hpp"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
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
    SPDLOG_INFO("Attaching runtime");
    auto item = bpftime_map_lookup_elem(10, &tid); // thread map
    mem_stats stats;
    alloc_info alloc_info1;
    proc_info proc_info1;
    proc_info thread_info1;
    for (int i = 0; i < 11; i++) {
        int key = 0;
        int key1 = 0;
        auto item1 = bpftime_map_get_next_key(i, &key1, &key); // process map
        // SPDLOG_INFO("Process map key: {} {} {}", key1, key, tid);
        auto item2 = bpftime_map_lookup_elem(i, &key); // allocs map
        if (i == 6 && item2 != nullptr){
            stats = *((mem_stats *)item2);
        }
        if (i == 7 && item2 != nullptr){
            alloc_info1 = *((alloc_info *)item2);
        }
        if (i == 9 && item2 != nullptr){
            proc_info1 = *((proc_info *)item2);
        }
        if (i == 10 && item2 != nullptr){
            thread_info1 = *((proc_info *)item2);
        }
    }
    printf("stats: %llu %llu %llu %llu %llu\n", stats.total_allocated, stats.total_freed, stats.current_usage, stats.allocation_count, stats.free_count);
    printf("alloc_info1: %llu %llu\n", alloc_info1.size, alloc_info1.address);
    controller->set_stats(stats);
    controller->set_alloc_info(alloc_info1);
    // auto item_alloc = bpftime_map_lookup_elem(7, &tid); // allocs map
    // if (item_alloc != nullptr) {
    //     SPDLOG_INFO("Allocs map key: {}", ((alloc_info *)item_alloc)->size);
    // }
    // auto item_stats = bpftime_map_lookup_elem(6, &tid); // stats map
    // if (item_stats != nullptr) {
    //     SPDLOG_INFO("Allocs map key: {}", ((mem_stats *)item_stats)->total_allocated);
    // }

    controller->set_process_info(proc_info1);
    controller->set_thread_info(thread_info1);
    // auto item_process = bpftime_map_lookup_elem(9, &tid); // process map
    // if (item_process != nullptr) {
    //     SPDLOG_INFO("Allocs map key: {}", ((proc_info *)item_process)->mem_info.current_brk);
    // }
    // auto item_thread = bpftime_map_lookup_elem(10, &tid); // thread map
    // if (item_thread != nullptr) {
    //     SPDLOG_INFO("Allocs map key: {}", ((proc_info *)item_thread)->mem_info.current_brk);
    // }
    return 0;
}

