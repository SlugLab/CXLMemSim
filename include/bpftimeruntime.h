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

class BpfTimeRuntime {
public:
    BpfTimeRuntime(pid_t, std::string);
    ~BpfTimeRuntime();

    int read(CXLController *, BPFTimeRuntimeElem *);

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
    struct mem_info mem_info;
};

#endif
