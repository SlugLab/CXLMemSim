#include <linux/filter.h>
#include <linux/ptrace.h>
#include <linux/version.h>
#include <uapi/linux/bpf.h>

/* helper macro to place programs, maps, license in
 * different sections in elf_bpf file. Section names
 * are interpreted by elf_bpf loader
 */
#define SEC(NAME) __attribute__((section(NAME), used))

/* helper functions called from eBPF programs written in C */
static int (*bpf_probe_read)(void *dst, int size, void *unsafe_ptr) =
    (void *) BPF_FUNC_probe_read;
static unsigned long long (*bpf_ktime_get_ns)(void) =
    (void *) BPF_FUNC_ktime_get_ns;
static int (*bpf_trace_printk)(const char *fmt, int fmt_size, ...) =
    (void *) BPF_FUNC_trace_printk;
static unsigned long long (*bpf_get_current_pid_tgid)(void) =
    (void *) BPF_FUNC_get_current_pid_tgid;
/* a helper structure used by eBPF C program
 * to describe map attributes to elf_bpf loader
 */
struct bpf_map_def {
    unsigned int type;
    unsigned int key_size;
    unsigned int value_size;
    unsigned int max_entries;
    unsigned int map_flags;
    unsigned int inner_map_idx;
};
#define PT_REGS_PARM1(x) ((x)->di)
#define PT_REGS_PARM2(x) ((x)->si)
SEC("kprobe/__x64_sys_mmap")
int bpf_prog1(struct pt_regs *ctx) {
    long size;
    long address;
    char fmt[] = "%d %ld %llu\n";
    u32 pid = bpf_get_current_pid_tgid();
    bpf_probe_read(&size, sizeof(size), (void *)&PT_REGS_PARM2(ctx));
    bpf_probe_read(&address, sizeof(address), (void *)&PT_REGS_PARM1(ctx));

    bpf_trace_printk(fmt, sizeof(fmt), size, address, bpf_ktime_get_ns());

    return 0;
}

char _license[] SEC("license") = "GPL";
u32 _version SEC("version") = LINUX_VERSION_CODE;
