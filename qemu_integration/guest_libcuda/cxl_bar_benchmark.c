/*
 * CXL Type 2 BAR Memory Bandwidth Benchmark
 * Measures raw CXL interconnect performance via direct BAR MMIO access.
 * Bypasses CUDA/hetGPU - purely tests CXL.io / CXL.mem paths.
 *
 * Compile: gcc -O2 -o cxl_bar_benchmark cxl_bar_benchmark.c -lrt -lpthread
 * Usage:   sudo ./cxl_bar_benchmark
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>

#include "cxl_gpu_cmd.h"

/* ── Device Discovery ──────────────────────────────────────────── */

#define CXL_TYPE2_VENDOR  0x8086
#define CXL_TYPE2_DEVICE  0x0d92
#define MAX_DEVICES       4

typedef struct {
    char bdf[32];            /* PCI BDF string */
    int  bar2_fd;
    int  bar4_fd;
    volatile uint8_t *bar2;  /* Register + data region */
    volatile uint8_t *bar4;  /* Bulk transfer region */
    size_t bar2_size;
    size_t bar4_size;
} cxl_dev_t;

static int g_num_devices = 0;
static cxl_dev_t g_devs[MAX_DEVICES];

static uint16_t read_pci_id(const char *bdf, const char *which)
{
    char path[256], buf[32];
    int fd;
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", bdf, which);
    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return (uint16_t)strtol(buf, NULL, 16);
}

static size_t bar_range(const char *bdf, int bar_index)
{
    char path[256], line[128];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource", bdf);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    for (int i = 0; i <= bar_index; i++) {
        if (!fgets(line, sizeof(line), fp)) { fclose(fp); return 0; }
    }
    fclose(fp);
    uint64_t start, end, flags;
    if (sscanf(line, "0x%lx 0x%lx 0x%lx", &start, &end, &flags) != 3)
        return 0;
    if (end <= start) return 0;
    return (size_t)(end - start + 1);
}

static volatile uint8_t *map_bar(const char *bdf, int bar_index, size_t size, int *fd_out)
{
    char path[256];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource%d", bdf, bar_index);
    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) { fprintf(stderr, "  Cannot open %s: %s\n", path, strerror(errno)); return NULL; }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return NULL; }
    *fd_out = fd;
    return (volatile uint8_t *)map;
}

static void enable_device(const char *bdf)
{
    char path[256];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/enable", bdf);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { write(fd, "1", 1); close(fd); }
}

static int discover_devices(void)
{
    DIR *dir = opendir("/sys/bus/pci/devices");
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) && g_num_devices < MAX_DEVICES) {
        if (ent->d_name[0] == '.') continue;
        if (read_pci_id(ent->d_name, "vendor") != CXL_TYPE2_VENDOR) continue;
        if (read_pci_id(ent->d_name, "device") != CXL_TYPE2_DEVICE) continue;

        cxl_dev_t *d = &g_devs[g_num_devices];
        strncpy(d->bdf, ent->d_name, sizeof(d->bdf) - 1);
        enable_device(d->bdf);

        d->bar2_size = bar_range(d->bdf, 2);
        d->bar4_size = bar_range(d->bdf, 4);
        if (d->bar2_size == 0) d->bar2_size = CXL_GPU_CMD_REG_SIZE;

        d->bar2 = map_bar(d->bdf, 2, d->bar2_size, &d->bar2_fd);
        if (!d->bar2) continue;

        /* Validate magic */
        uint32_t magic = *(volatile uint32_t *)(d->bar2 + CXL_GPU_REG_MAGIC);
        if (magic != CXL_GPU_MAGIC) {
            fprintf(stderr, "  %s: bad magic 0x%x\n", d->bdf, magic);
            munmap((void *)d->bar2, d->bar2_size);
            close(d->bar2_fd);
            continue;
        }

        /* BAR4 is optional */
        if (d->bar4_size > 0) {
            d->bar4 = map_bar(d->bdf, 4, d->bar4_size, &d->bar4_fd);
        }

        printf("  [%d] %s  BAR2=%zu KB  BAR4=%zu MB  magic=OK\n",
               g_num_devices, d->bdf,
               d->bar2_size / 1024,
               d->bar4_size / (1024 * 1024));
        g_num_devices++;
    }
    closedir(dir);
    return g_num_devices;
}

static void cleanup_devices(void)
{
    for (int i = 0; i < g_num_devices; i++) {
        cxl_dev_t *d = &g_devs[i];
        if (d->bar2) { munmap((void *)d->bar2, d->bar2_size); close(d->bar2_fd); }
        if (d->bar4) { munmap((void *)d->bar4, d->bar4_size); close(d->bar4_fd); }
    }
}

/* ── Timing helpers ────────────────────────────────────────────── */

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static double time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* ── Benchmark: Register Latency ───────────────────────────────── */

static void bench_register_latency(cxl_dev_t *d)
{
    const int ITERS = 100000;
    volatile uint32_t *regs = (volatile uint32_t *)d->bar2;

    printf("\n--- Register Latency (device %s) ---\n", d->bdf);

    /* Read latency (read MAGIC register) */
    __sync_synchronize();
    double t0 = time_ns();
    volatile uint32_t sink = 0;
    for (int i = 0; i < ITERS; i++) {
        sink = regs[CXL_GPU_REG_MAGIC / 4];
    }
    double t1 = time_ns();
    (void)sink;
    printf("  32-bit read  latency:  %7.1f ns/op  (%d ops)\n",
           (t1 - t0) / ITERS, ITERS);

    /* Write latency (write PARAM0 register) */
    __sync_synchronize();
    t0 = time_ns();
    for (int i = 0; i < ITERS; i++) {
        *(volatile uint32_t *)(d->bar2 + CXL_GPU_REG_PARAM0) = (uint32_t)i;
        __sync_synchronize();
    }
    t1 = time_ns();
    printf("  32-bit write latency:  %7.1f ns/op  (%d ops)\n",
           (t1 - t0) / ITERS, ITERS);

    /* 64-bit read latency */
    __sync_synchronize();
    t0 = time_ns();
    volatile uint64_t sink64 = 0;
    for (int i = 0; i < ITERS; i++) {
        sink64 = *(volatile uint64_t *)(d->bar2 + CXL_GPU_REG_TOTAL_MEM);
    }
    t1 = time_ns();
    (void)sink64;
    printf("  64-bit read  latency:  %7.1f ns/op  (%d ops)\n",
           (t1 - t0) / ITERS, ITERS);

    /* 64-bit write latency */
    __sync_synchronize();
    t0 = time_ns();
    for (int i = 0; i < ITERS; i++) {
        *(volatile uint64_t *)(d->bar2 + CXL_GPU_REG_PARAM0) = (uint64_t)i;
        __sync_synchronize();
    }
    t1 = time_ns();
    printf("  64-bit write latency:  %7.1f ns/op  (%d ops)\n",
           (t1 - t0) / ITERS, ITERS);
}

/* ── Benchmark: Data Region Bandwidth ──────────────────────────── */

static void bench_data_region_bw(cxl_dev_t *d)
{
    volatile uint8_t *data = d->bar2 + CXL_GPU_DATA_OFFSET;
    size_t region_size = CXL_GPU_DATA_SIZE;
    if (CXL_GPU_DATA_OFFSET + region_size > d->bar2_size) {
        region_size = d->bar2_size - CXL_GPU_DATA_OFFSET;
    }
    if (region_size == 0 || region_size > d->bar2_size) {
        printf("\n--- Data Region BW: skipped (region not mapped) ---\n");
        return;
    }

    printf("\n--- Data Region Bandwidth (device %s, %zu KB) ---\n",
           d->bdf, region_size / 1024);

    uint8_t *host_buf = malloc(region_size);
    if (!host_buf) return;
    memset(host_buf, 0xCD, region_size);

    /* Sizes to test */
    size_t sizes[] = { 4096, 16384, 65536, 262144, region_size };
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int si = 0; si < nsizes; si++) {
        size_t sz = sizes[si];
        if (sz > region_size) sz = region_size;
        int iters = (sz <= 65536) ? 200 : 50;

        /* Write bandwidth (host → BAR) */
        __sync_synchronize();
        double t0 = time_ns();
        for (int it = 0; it < iters; it++) {
            for (size_t off = 0; off < sz; off += 8) {
                *(volatile uint64_t *)(data + off) = *(uint64_t *)(host_buf + off);
            }
            __sync_synchronize();
        }
        double t1 = time_ns();
        double wr_bw = ((double)sz * iters) / ((t1 - t0) / 1e9) / (1024.0 * 1024);

        /* Read bandwidth (BAR → host) */
        __sync_synchronize();
        t0 = time_ns();
        for (int it = 0; it < iters; it++) {
            for (size_t off = 0; off < sz; off += 8) {
                *(uint64_t *)(host_buf + off) = *(volatile uint64_t *)(data + off);
            }
            __sync_synchronize();
        }
        t1 = time_ns();
        double rd_bw = ((double)sz * iters) / ((t1 - t0) / 1e9) / (1024.0 * 1024);

        printf("  %7zu B:  write %8.1f MB/s   read %8.1f MB/s  (%d iters)\n",
               sz, wr_bw, rd_bw, iters);
    }

    free(host_buf);
}

/* ── Benchmark: BAR4 Bulk Transfer Bandwidth ───────────────────── */

static void bench_bar4_bulk_bw(cxl_dev_t *d)
{
    if (!d->bar4 || d->bar4_size == 0) {
        printf("\n--- BAR4 Bulk BW: skipped (BAR4 not mapped) ---\n");
        return;
    }

    size_t test_size = d->bar4_size;
    if (test_size > 64 * 1024 * 1024) test_size = 64 * 1024 * 1024;

    printf("\n--- BAR4 Bulk Transfer Bandwidth (device %s, %zu MB) ---\n",
           d->bdf, test_size / (1024 * 1024));

    uint8_t *host_buf = malloc(test_size);
    if (!host_buf) return;
    memset(host_buf, 0xEF, test_size);

    size_t sizes[] = { 1024*1024, 4*1024*1024, 16*1024*1024, test_size };
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int si = 0; si < nsizes; si++) {
        size_t sz = sizes[si];
        if (sz > test_size) sz = test_size;
        int iters = (sz <= 4*1024*1024) ? 20 : 5;

        /* Write */
        __sync_synchronize();
        double t0 = time_ns();
        for (int it = 0; it < iters; it++) {
            for (size_t off = 0; off < sz; off += 8) {
                *(volatile uint64_t *)(d->bar4 + off) = *(uint64_t *)(host_buf + off);
            }
            __sync_synchronize();
        }
        double t1 = time_ns();
        double wr_bw = ((double)sz * iters) / ((t1 - t0) / 1e9) / (1024.0 * 1024);

        /* Read */
        __sync_synchronize();
        t0 = time_ns();
        for (int it = 0; it < iters; it++) {
            for (size_t off = 0; off < sz; off += 8) {
                *(uint64_t *)(host_buf + off) = *(volatile uint64_t *)(d->bar4 + off);
            }
            __sync_synchronize();
        }
        t1 = time_ns();
        double rd_bw = ((double)sz * iters) / ((t1 - t0) / 1e9) / (1024.0 * 1024);

        printf("  %4zu MB:  write %8.1f MB/s   read %8.1f MB/s  (%d iters)\n",
               sz / (1024*1024), wr_bw, rd_bw, iters);
    }

    free(host_buf);
}

/* ── Benchmark: Command Dispatch Latency ──────────────────────── */

static void bench_cmd_latency(cxl_dev_t *d)
{
    printf("\n--- Command Dispatch Latency (device %s) ---\n", d->bdf);

    volatile uint32_t *cmd_reg    = (volatile uint32_t *)(d->bar2 + CXL_GPU_REG_CMD);
    volatile uint32_t *status_reg = (volatile uint32_t *)(d->bar2 + CXL_GPU_REG_CMD_STATUS);
    volatile uint32_t *result_reg = (volatile uint32_t *)(d->bar2 + CXL_GPU_REG_CMD_RESULT);

    /* NOP command latency */
    const int ITERS = 10000;
    double t0 = time_ns();
    for (int i = 0; i < ITERS; i++) {
        *cmd_reg = CXL_GPU_CMD_NOP;
        __sync_synchronize();
        int timeout = 100000;
        while (timeout-- > 0) {
            uint32_t st = *status_reg;
            if (st == CXL_GPU_CMD_STATUS_COMPLETE || st == CXL_GPU_CMD_STATUS_ERROR)
                break;
        }
    }
    double t1 = time_ns();
    printf("  NOP cmd round-trip:    %7.1f ns/op  (%d ops)\n",
           (t1 - t0) / ITERS, ITERS);

    /* GET_DEVICE_COUNT latency */
    t0 = time_ns();
    for (int i = 0; i < ITERS; i++) {
        *cmd_reg = CXL_GPU_CMD_GET_DEVICE_COUNT;
        __sync_synchronize();
        int timeout = 100000;
        while (timeout-- > 0) {
            uint32_t st = *status_reg;
            if (st == CXL_GPU_CMD_STATUS_COMPLETE || st == CXL_GPU_CMD_STATUS_ERROR)
                break;
        }
    }
    t1 = time_ns();
    printf("  GET_DEV_COUNT cmd:     %7.1f ns/op  (%d ops)\n",
           (t1 - t0) / ITERS, ITERS);

    (void)result_reg;
}

/* ── Benchmark: Stride / Random Access Pattern ─────────────────── */

static void bench_access_patterns(cxl_dev_t *d)
{
    volatile uint8_t *data = d->bar2 + CXL_GPU_DATA_OFFSET;
    size_t region_size = CXL_GPU_DATA_SIZE;
    if (CXL_GPU_DATA_OFFSET + region_size > d->bar2_size)
        region_size = d->bar2_size - CXL_GPU_DATA_OFFSET;
    if (region_size < 4096) return;

    printf("\n--- Access Pattern Sensitivity (device %s, %zu KB) ---\n",
           d->bdf, region_size / 1024);

    /* Cap to 256KB for random-access test to keep runtime sane */
    size_t test_size = region_size;
    if (test_size > 256 * 1024) test_size = 256 * 1024;
    int n_qwords = test_size / 8;

    /* Sequential stride read */
    int strides[] = { 8, 64, 256, 4096 };
    for (int si = 0; si < 4; si++) {
        int stride = strides[si];
        int accesses = test_size / stride;
        if (accesses < 100) accesses = 100;

        volatile uint64_t sink = 0;
        __sync_synchronize();
        double t0 = time_ns();
        for (int i = 0; i < accesses; i++) {
            size_t off = ((size_t)i * stride) % test_size;
            off &= ~7UL; /* align to 8 bytes */
            sink += *(volatile uint64_t *)(data + off);
        }
        double t1 = time_ns();
        (void)sink;
        printf("  stride=%5d B:  %7.1f ns/access  (%d accesses)\n",
               stride, (t1 - t0) / accesses, accesses);
    }

    /* Pseudo-random access (LCG) */
    {
        int accesses = n_qwords;
        if (accesses > 50000) accesses = 50000;
        volatile uint64_t sink = 0;
        uint32_t idx = 0x12345678;
        __sync_synchronize();
        double t0 = time_ns();
        for (int i = 0; i < accesses; i++) {
            idx = idx * 1103515245 + 12345;
            size_t off = (idx % n_qwords) * 8;
            sink += *(volatile uint64_t *)(data + off);
        }
        double t1 = time_ns();
        (void)sink;
        printf("  random 8B:          %7.1f ns/access  (%d accesses)\n",
               (t1 - t0) / accesses, accesses);
    }
}

/* ── Benchmark: Dual-Device Concurrent Access ──────────────────── */

typedef struct {
    cxl_dev_t *dev;
    size_t     size;
    double     write_bw;   /* result: MB/s */
    double     read_bw;    /* result: MB/s */
} thread_arg_t;

static void *device_bw_thread(void *arg)
{
    thread_arg_t *ta = (thread_arg_t *)arg;
    volatile uint8_t *data = ta->dev->bar2 + CXL_GPU_DATA_OFFSET;
    size_t sz = ta->size;
    int iters = 50;

    uint8_t *host_buf = malloc(sz);
    memset(host_buf, 0xAA, sz);

    /* Write */
    __sync_synchronize();
    double t0 = time_ns();
    for (int it = 0; it < iters; it++) {
        for (size_t off = 0; off < sz; off += 8)
            *(volatile uint64_t *)(data + off) = *(uint64_t *)(host_buf + off);
        __sync_synchronize();
    }
    double t1 = time_ns();
    ta->write_bw = ((double)sz * iters) / ((t1 - t0) / 1e9) / (1024.0 * 1024);

    /* Read */
    __sync_synchronize();
    t0 = time_ns();
    for (int it = 0; it < iters; it++) {
        for (size_t off = 0; off < sz; off += 8)
            *(uint64_t *)(host_buf + off) = *(volatile uint64_t *)(data + off);
        __sync_synchronize();
    }
    t1 = time_ns();
    ta->read_bw = ((double)sz * iters) / ((t1 - t0) / 1e9) / (1024.0 * 1024);

    free(host_buf);
    return NULL;
}

static void bench_dual_device(void)
{
    if (g_num_devices < 2) {
        printf("\n--- Dual-Device Concurrent: skipped (need 2 devices) ---\n");
        return;
    }

    size_t sz = CXL_GPU_DATA_SIZE;
    if (sz > 65536) sz = 65536;

    printf("\n--- Dual-Device Concurrent Access (%zu KB each) ---\n", sz / 1024);

    /* First: sequential baseline for each device */
    thread_arg_t args[2];
    for (int i = 0; i < 2; i++) {
        args[i].dev  = &g_devs[i];
        args[i].size = sz;
        device_bw_thread(&args[i]);
        printf("  Device %d alone:  write %8.1f MB/s   read %8.1f MB/s\n",
               i, args[i].write_bw, args[i].read_bw);
    }

    /* Then: concurrent */
    pthread_t threads[2];
    for (int i = 0; i < 2; i++) {
        args[i].dev  = &g_devs[i];
        args[i].size = sz;
    }
    for (int i = 0; i < 2; i++)
        pthread_create(&threads[i], NULL, device_bw_thread, &args[i]);
    for (int i = 0; i < 2; i++)
        pthread_join(threads[i], NULL);

    double agg_wr = args[0].write_bw + args[1].write_bw;
    double agg_rd = args[0].read_bw  + args[1].read_bw;
    printf("  Concurrent:       write %8.1f MB/s   read %8.1f MB/s  (aggregate)\n",
           agg_wr, agg_rd);
    printf("  Scaling:          write  %.2fx          read  %.2fx\n",
           agg_wr / args[0].write_bw, agg_rd / args[0].read_bw);
}

/* ── Main ──────────────────────────────────────────────────────── */

int main(void)
{
    printf("==========================================================\n");
    printf("  CXL Type 2 BAR Memory Bandwidth Benchmark\n");
    printf("==========================================================\n\n");

    printf("Discovering CXL Type 2 devices (vendor=0x%04x device=0x%04x)...\n",
           CXL_TYPE2_VENDOR, CXL_TYPE2_DEVICE);
    if (discover_devices() == 0) {
        fprintf(stderr, "No CXL Type 2 devices found.\n");
        return 1;
    }
    printf("Found %d device(s)\n", g_num_devices);

    for (int i = 0; i < g_num_devices; i++) {
        cxl_dev_t *d = &g_devs[i];

        /* Print identity */
        uint32_t status = *(volatile uint32_t *)(d->bar2 + CXL_GPU_REG_STATUS);
        uint32_t caps   = *(volatile uint32_t *)(d->bar2 + CXL_GPU_REG_CAPS);
        printf("\n  Device %d status=0x%x caps=0x%x\n", i, status, caps);

        bench_register_latency(d);
        bench_cmd_latency(d);
        bench_data_region_bw(d);
        bench_access_patterns(d);
        bench_bar4_bulk_bw(d);
    }

    bench_dual_device();

    printf("\n==========================================================\n");
    printf("  Benchmark complete\n");
    printf("==========================================================\n");

    cleanup_devices();
    return 0;
}
