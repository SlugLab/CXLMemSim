#include "vmlinux.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

SEC("kprobe/__x64_sys_munmap")
int munmap_init(struct pt_regs *ctx) {
    long size;
    long address;
    char fmt[] = "munmap %ld %ld %u\n";
    u32 pid = bpf_get_current_pid_tgid();
    bpf_probe_read(&size, sizeof(size), (void *)&PT_REGS_PARM2(ctx));
    bpf_probe_read(&address, sizeof(address), (void *)&PT_REGS_PARM1(ctx));
    if (size > 0) {
        bpf_trace_printk(fmt, sizeof(fmt), size, address, pid);
    }
    return 0;
}
SEC("kprobe/__x64_sys_brk")
int brk_init(struct pt_regs *ctx) {
    long address;
    char fmt[] = "brk %ld %u\n";
    u32 pid = bpf_get_current_pid_tgid();
    bpf_probe_read(&address, sizeof(address), (void *)&PT_REGS_PARM1(ctx));
    bpf_trace_printk(fmt, sizeof(fmt), address, pid);
    return 0;
}
SEC("kretprobe/__x64_sys_brk")
int brk_finish(struct pt_regs *ctx) {
    int size;
    char fmt[] = "brkret %d %u\n";
    u32 pid = bpf_get_current_pid_tgid();
    bpf_probe_read(&size, sizeof(size), (void *)&PT_REGS_PARM1(ctx));
    if (size > 0) {
        bpf_trace_printk(fmt, sizeof(fmt), size, pid);
    }
    return 0;
}
SEC("kprobe/__x64_sys_sbrk")
int sbrk_init(struct pt_regs *ctx) {
    int size;
    char fmt[] = "sbrkret %d %u\n";
    u32 pid = bpf_get_current_pid_tgid();
    bpf_probe_read(&size, sizeof(size), (void *)&PT_REGS_PARM1(ctx));
    if (size > 0) {
        bpf_trace_printk(fmt, sizeof(fmt), size, pid);
    }
    return 0;
}
SEC("kretprobe/__x64_sys_sbrk")
int sbrk_finish(struct pt_regs *ctx) {
    long address;
    char fmt[] = "sbrkret %ld %u\n";
    u32 pid = bpf_get_current_pid_tgid();
    bpf_probe_read(&address, sizeof(address), (void *)&PT_REGS_PARM1(ctx));
    bpf_trace_printk(fmt, sizeof(fmt), address, pid);

    return 0;
}
char _license[] SEC("license") = "GPL";
