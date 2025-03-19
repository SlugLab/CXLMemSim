/*
 * CXLMemSim controller
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#ifndef CXLMEMSIM_BPFTIME_RUNTIME_H
#define CXLMEMSIM_BPFTIME_RUNTIME_H

#ifdef __cplusplus
#include "cxlcontroller.h"
#include <linux/bpf.h>
#include <string>
#include <sys/types.h>
#include "bpftime_config.hpp"
#include "bpftime_logger.hpp"
#include "bpftime_shm.hpp"
template <typename K, typename V>
class BPFUpdater {
public:
    int map_fd;
    BPFUpdater(int map_fd) : map_fd(map_fd) {}

    void update(K key, V value) {
        int key1 = 0;
        bpftime_map_get_next_key(map_fd, &key1, &key); // process map
        auto item2 = (struct proc_info *)bpftime_map_lookup_elem(map_fd, &key); // allocs map
        item2->sleep_time = (uint64_t)value;
        int ret = bpftime_map_update_elem(map_fd, &key, item2, BPF_ANY);
        if (ret != 0) {
            SPDLOG_ERROR("Error updating map: {}\n", strerror(errno));
            throw std::runtime_error("Error updating the bpf map");
        }
    }

    bool get(K key) {
        int key1 = 0;
        bpftime_map_get_next_key(map_fd, &key1, &key); // process map
        auto item2 = (struct proc_info *)bpftime_map_lookup_elem(map_fd, &key); // allocs map
        if (item2 == nullptr) {
            return false;
        }
        return item2->is_locked;
    }
};
class BpfTimeRuntime {
public:
    BpfTimeRuntime(pid_t, std::string);
    ~BpfTimeRuntime();

    int read(CXLController *, BPFTimeRuntimeElem *);
    BPFUpdater<uint64_t,uint64_t> *updater;
    pid_t tid;
};


#define u64 unsigned long long
#define u32 unsigned int
#else
#include "vmlinux.h"
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#endif
// 定义内存统计结构体
struct mem_stats {
    u64 total_allocated; // 总分配量
    u64 total_freed; // 总释放量
    u64 current_usage; // 当前使用量
    u64 allocation_count; // 分配次数
    u64 free_count; // 释放次数
};

// 定义分配信息结构体
struct alloc_info {
    u64 size; // 分配的大小
    u64 address; // 分配的地址
};

struct mem_info {
    u64 current_brk; // 当前 brk 位置
    u64 total_allocated; // 总分配量
    u64 total_freed; // 总释放量
};
struct proc_info {
    u32 parent_pid; // 父进程 ID
    u64 create_time; // 创建时间
    u64 thread_count; // 线程数量（仅对进程有效）
    u64 current_pid; // 当前进程 ID
    u64 current_tid; // 当前线程 ID
    u64 sleep_time; // 睡眠时间
    bool is_locked; // 是否锁定
    struct mem_info mem_info;
};

// 线程创建参数
struct thread_create_args {
	void **thread_ptr;
	void *attr;
	void *start_routine;
	void *arg;
};
#endif
