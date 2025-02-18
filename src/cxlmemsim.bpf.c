#define BPF_NO_GLOBAL_DATA
/*
 * CXLMemSim bpfhook
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "../include/bpftimeruntime.h"

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 100000);
    __type(key, u64);
    __type(value, struct alloc_info);
} allocs_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, struct mem_stats);
} stats_map SEC(".maps");

// 存储线程信息的 map
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 102400);
    __type(key, u32); // tid as key
    __type(value, struct proc_info);
} thread_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, struct proc_info);
} process_map SEC(".maps");

// 用于处理多线程同步的自旋锁映射
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, u32);
} locks SEC(".maps");

SEC("uprobe//lib/x86_64-linux-gnu/libc.so.6:malloc")
int malloc_entry(struct pt_regs *ctx) {
    u64 size;
    u32 pid = bpf_get_current_pid_tgid();
    u64 pid_tgid = bpf_get_current_pid_tgid();

    // 读取分配大小参数
    bpf_probe_read_user(&size, sizeof(size), (void *)&PT_REGS_PARM1(ctx));

    // 更新统计信息
    struct mem_stats *stats, zero_stats = {};
    stats = bpf_map_lookup_elem(&stats_map, &pid);
    if (!stats) {
        bpf_map_update_elem(&stats_map, &pid, &zero_stats, BPF_ANY);
        stats = bpf_map_lookup_elem(&stats_map, &pid);
        if (!stats)
            return 0;
    }

    // 记录请求的大小
    struct alloc_info info = {
        .size = size,
    };
    bpf_map_update_elem(&allocs_map, &pid_tgid, &info, BPF_ANY);
    return 0;
}

SEC("uretprobe//lib/x86_64-linux-gnu/libc.so.6:malloc")
int BPF_KRETPROBE(malloc_return, void *address) {
    u32 pid = bpf_get_current_pid_tgid();
    u64 pid_tgid = bpf_get_current_pid_tgid();
    // bpf_printk("malloc return address: %lx\n", address);

    // 2. 添加调试日志

    struct alloc_info *info = bpf_map_lookup_elem(&allocs_map, &pid_tgid);
    if (!info) {
        bpf_printk("alloc info not found for pid_tgid: %llu\n", pid_tgid);
        return 0;
    }

    if (address) {
        struct mem_stats *stats = bpf_map_lookup_elem(&stats_map, &pid);
        if (stats) {
            stats->total_allocated += info->size;
            stats->current_usage += info->size;
            stats->allocation_count += 1;
            // __sync_fetch_and_add(&stats->total_allocated,
            // 		     info->size);
            // __sync_fetch_and_add(&stats->current_usage, info->size);
            // __sync_fetch_and_add(&stats->allocation_count, 1);

            info->address = (u64)address;
            bpf_map_update_elem(&allocs_map, &address, info, BPF_ANY);
        }
    }

    return 0;
}
SEC("uprobe//lib/x86_64-linux-gnu/libc.so.6:calloc")
int calloc_entry(struct pt_regs *ctx) {
    u64 size;
    u32 pid = bpf_get_current_pid_tgid();
    u64 pid_tgid = bpf_get_current_pid_tgid();

    bpf_probe_read_user(&size, sizeof(size), (void *)&PT_REGS_PARM1(ctx));
   // 更新统计信息
    struct mem_stats *stats, zero_stats = {};
    stats = bpf_map_lookup_elem(&stats_map, &pid);
    if (!stats) {
        bpf_map_update_elem(&stats_map, &pid, &zero_stats, BPF_ANY);
        stats = bpf_map_lookup_elem(&stats_map, &pid);
        if (!stats)
            return 0;
    }

    // 记录请求的大小
    struct alloc_info info = {
        .size = size,
    };
    bpf_map_update_elem(&allocs_map, &pid_tgid, &info, BPF_ANY);
    return 0;
}

SEC("uretprobe//lib/x86_64-linux-gnu/libc.so.6:calloc")
int calloc_return(struct pt_regs *ctx) {
    u32 pid = bpf_get_current_pid_tgid();
    u64 pid_tgid = bpf_get_current_pid_tgid();

    struct alloc_info *info = bpf_map_lookup_elem(&allocs_map, &pid_tgid);
    if (!info) {
        bpf_printk("alloc info not found for pid_tgid: %llu\n", pid_tgid);
        return 0;
    }

    if (info->address) {
        struct mem_stats *stats = bpf_map_lookup_elem(&stats_map, &pid);
        if (stats) {
            stats->total_freed += info->size;
            stats->current_usage -= info->size;
            stats->free_count += 1;
        }
    }

    bpf_map_delete_elem(&allocs_map, &pid_tgid);

    return 0;
}

SEC("uprobe//lib/x86_64-linux-gnu/libc.so.6:free")
int free_entry(struct pt_regs *ctx) {
    u64 address;
    u32 pid = bpf_get_current_pid_tgid();

    bpf_probe_read_user(&address, sizeof(address), (void *)&PT_REGS_PARM1(ctx));

    if (!address)
        return 0;

    struct alloc_info *info = bpf_map_lookup_elem(&allocs_map, &address);
    if (!info)
        return 0;

    struct mem_stats *stats = bpf_map_lookup_elem(&stats_map, &pid);
    if (stats) {
        // __sync_fetch_and_add(&stats->total_freed, info->size);
        stats->total_freed += info->size;
        // __sync_fetch_and_sub(&stats->current_usage, info->size);
        stats->current_usage -= info->size;
        // __sync_fetch_and_add(&stats->free_count, 1);
        stats->free_count += 1;
    }

    bpf_map_delete_elem(&allocs_map, &address);

    return 0;
}

// 辅助函数：获取或创建 mem_info
static struct mem_info *get_or_create_mem_info(u32 pid) {
    struct proc_info *proc_info = bpf_map_lookup_elem(&process_map, &pid);
    struct mem_info *info = &proc_info->mem_info;
    if (!info) {
        struct mem_info new_info = {};
        bpf_map_update_elem(&process_map, &pid, &new_info, BPF_ANY);
        info = bpf_map_lookup_elem(&process_map, &pid);
    }
    return info;
}

// 简单的自旋锁实现
static bool try_lock(u32 pid) {
    u32 locked = 1;
    u32 *current = bpf_map_lookup_elem(&locks, &pid);
    if (!current) {
        bpf_map_update_elem(&locks, &pid, &locked, BPF_ANY);
        return true;
    }
    return false;
}

static void unlock(u32 pid) { bpf_map_delete_elem(&locks, &pid); }

SEC("uprobe//lib/x86_64-linux-gnu/libc.so.6:brk")
int brk_entry(struct pt_regs *ctx) {
    u64 addr;
    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 pid = pid_tgid >> 32;
    u32 tid = (u32)pid_tgid;

    // 尝试获取锁
    u32 locked = 1;
    u32 *current = bpf_map_lookup_elem(&locks, &pid);
    if (!current) {
        bpf_map_update_elem(&locks, &pid, &locked, BPF_ANY);
    } else {
        // 已经有锁了，直接返回
        return 0;
    }

    // 读取 brk 参数
    bpf_probe_read(&addr, sizeof(addr), (void *)&PT_REGS_PARM1(ctx));

    // 获取或创建进程信息
    struct proc_info *proc_info = bpf_map_lookup_elem(&process_map, &pid);
    if (!proc_info) {
        // 如果不存在，创建新的进程信息
        struct proc_info new_proc_info = {
            .mem_info = {.current_brk = addr, .total_allocated = 0, .total_freed = 0},
            .current_pid = pid,
            .current_tid = tid,
        };
        bpf_map_update_elem(&process_map, &pid, &new_proc_info, BPF_ANY);
    } else {
        // 如果存在，更新 brk 值
        proc_info->mem_info.current_brk = addr;
    }

    // 释放锁
    bpf_map_delete_elem(&locks, &pid);

    return 0;
}

SEC("uretprobe//lib/x86_64-linux-gnu/libc.so.6:brk")
int brk_return(struct pt_regs *ctx) {
    u64 ret = PT_REGS_RC(ctx);
    u32 pid = bpf_get_current_pid_tgid();

    // 尝试获取锁
    u32 locked = 1;
    u32 *current = bpf_map_lookup_elem(&locks, &pid);
    if (!current) {
        bpf_map_update_elem(&locks, &pid, &locked, BPF_ANY);
    } else {
        return 0;
    }

    struct proc_info *proc_info = bpf_map_lookup_elem(&process_map, &pid);
    if (proc_info) {
        u64 current_brk = proc_info->mem_info.current_brk;
        if (ret > current_brk) {
            // 内存增加
            u64 increase = ret - current_brk;
            proc_info->mem_info.total_allocated += increase;
        } else if (ret < current_brk) {
            // 内存释放
            u64 decrease = current_brk - ret;
            proc_info->mem_info.total_freed += decrease;
        }
        proc_info->mem_info.current_brk = ret;
    }

    // 释放锁
    bpf_map_delete_elem(&locks, &pid);

    return 0;
}

SEC("uprobe//lib/x86_64-linux-gnu/libc.so.6:execve")
int execve_entry(struct pt_regs *ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 pid = pid_tgid >> 32;
    u32 tid = (u32)pid_tgid;

    // 读取 execve 的参数
    const char *filename;
    char **argv;
    char **envp;

    bpf_probe_read(&filename, sizeof(filename), (void *)&PT_REGS_PARM1(ctx));
    bpf_probe_read(&argv, sizeof(argv), (void *)&PT_REGS_PARM2(ctx));
    bpf_probe_read(&envp, sizeof(envp), (void *)&PT_REGS_PARM3(ctx));

    // 创建新的进程信息
    struct proc_info proc_info = {.parent_pid = pid,
                                  .create_time = bpf_ktime_get_ns(),
                                  .thread_count = 1, // execve
                                  // 创建新进程时只有主线程
                                  .mem_info = {.total_allocated = 0, .total_freed = 0, .current_brk = 0}};

    // 保存进程信息
    bpf_map_update_elem(&process_map, &pid, &proc_info, BPF_ANY);

    // 清理旧的统计信息
    bpf_map_delete_elem(&stats_map, &pid);

    // 初始化新的统计信息
    struct mem_stats new_stats = {};
    bpf_map_update_elem(&stats_map, &pid, &new_stats, BPF_ANY);

    return 0;
}

// execve 的返回点探针
SEC("uretprobe//lib/x86_64-linux-gnu/libc.so.6:execve")
int execve_return(struct pt_regs *ctx) {
    int ret = PT_REGS_RC(ctx);
    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 pid = pid_tgid >> 32;

    // execve 成功返回 0
    if (ret == 0) {
        // 获取进程信息
        struct proc_info *proc_info = bpf_map_lookup_elem(&process_map, &pid);
        if (proc_info) {
            // 更新执行时间
            proc_info->create_time = bpf_ktime_get_ns();
        }
    } else {
        // execve 失败，清理之前创建的信息
        bpf_map_delete_elem(&process_map, &pid);
        bpf_map_delete_elem(&stats_map, &pid);
    }

    return 0;
}
SEC("uprobe//lib/x86_64-linux-gnu/libc.so.6:pthread_create")
int pthread_create_probe(struct pt_regs *ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 pid = pid_tgid >> 32;

    // 获取新线程的 ID（通过第一个参数）
    unsigned long *thread_ptr;
    bpf_probe_read(&thread_ptr, sizeof(thread_ptr), (void *)&PT_REGS_PARM1(ctx));

    if (thread_ptr) {
        // 创建新的线程信息
        struct proc_info thread_info = {
            .parent_pid = pid,
            .create_time = bpf_ktime_get_ns(),
        };

        // 更新线程计数
        struct proc_info *parent_info = bpf_map_lookup_elem(&process_map, &pid);
        if (parent_info) {
            // __sync_fetch_and_add(&parent_info->thread_count, 1);
            parent_info->thread_count += 1;
        }

        // 注意：我们需要在 return probe 中获取实际的线程 ID
        // 这里先保存父进程信息
        bpf_map_update_elem(&thread_map, &thread_ptr, &thread_info, BPF_ANY);
    }

    return 0;
}

SEC("uretprobe//lib/x86_64-linux-gnu/libc.so.6:pthread_create")
int pthread_create_ret(struct pt_regs *ctx) {
    int ret = PT_REGS_RC(ctx);

    // pthread_create 成功返回 0
    if (ret == 0) {
        // 这里可以添加额外的处理逻辑
        // 比如记录线程创建的结果
    }

    return 0;
}

SEC("uprobe//lib/x86_64-linux-gnu/libc.so.6:pthread_exit")
int pthread_exit_probe(struct pt_regs *ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 tid = (u32)pid_tgid;
    u32 pid = pid_tgid >> 32;

    // 查找并删除线程信息
    struct proc_info *thread_info = bpf_map_lookup_elem(&thread_map, &tid);
    if (thread_info) {
        // 更新父进程的线程计数
        struct proc_info *parent_info = bpf_map_lookup_elem(&process_map, &pid);
        if (parent_info) {
            // __sync_fetch_and_sub(&parent_info->thread_count, 1);
            parent_info->thread_count -= 1;
        }

        // 删除线程信息
        bpf_map_delete_elem(&thread_map, &tid);
    }

    return 0;
}

SEC("uprobe//lib/x86_64-linux-gnu/libc.so.6:fork")
int fork_probe(struct pt_regs *ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 parent_pid = pid_tgid >> 32;

    // 父进程信息会在 fork 返回时记录
    // 这里可以添加额外的前置处理逻辑

    return 0;
}

SEC("uretprobe//lib/x86_64-linux-gnu/libc.so.6:fork")
int fork_ret(struct pt_regs *ctx) {
    u32 child_pid = PT_REGS_RC(ctx);
    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 parent_pid = pid_tgid >> 32;
    u32 tid = child_pid;

    if (child_pid > 0) {
        // 创建新的进程信息
        struct proc_info proc_info = {
            .parent_pid = parent_pid,
            .create_time = bpf_ktime_get_ns(),
            .thread_count = 1, // 初始只有主线程
            .current_pid = child_pid,
            .current_tid = tid,
        };

        // 更新进程 map
        bpf_map_update_elem(&process_map, &child_pid, &proc_info, BPF_ANY);
    }

    return 0;
}

SEC("uprobe//lib/x86_64-linux-gnu/libc.so.6:exit")
int exit_probe(struct pt_regs *ctx) {
    u64 pid_tgid = bpf_get_current_pid_tgid();
    u32 pid = pid_tgid >> 32;

    // 清理进程信息
    bpf_map_delete_elem(&process_map, &pid);

    return 0;
}
char _license[] SEC("license") = "GPL";
