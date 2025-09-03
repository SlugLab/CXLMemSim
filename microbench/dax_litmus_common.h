// Shared helpers for DAX litmus tests
// Minimal deps, C11 atomics

#pragma once

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CACHELINE_SIZE 64

typedef enum { ROLE_A = 0, ROLE_B = 1 } role_t;

typedef struct {
    // Handshake
    _Atomic uint32_t magic;
    _Atomic uint32_t ready_a;
    _Atomic uint32_t ready_b;
    _Atomic uint32_t seq;
    _Atomic uint32_t flag;
    _Atomic uint64_t counter;
    uint8_t pad[64 - 4 - 4 - 4 - 4 - 8];
} __attribute__((aligned(CACHELINE_SIZE))) ctrl_block_t;

static inline const char *basename_of(const char *p) {
    const char *slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

static inline size_t read_sysfs_dax_size(const char *dax_path) {
    char sysfs[512];
    const char *base = basename_of(dax_path);
    snprintf(sysfs, sizeof(sysfs), "/sys/bus/dax/devices/%s/size", base);
    FILE *f = fopen(sysfs, "r");
    if (!f) return 0;
    unsigned long long v = 0ULL;
    if (fscanf(f, "%llu", &v) != 1) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return (size_t)v;
}

typedef struct {
    void *base;        // mapped base pointer
    size_t map_size;   // length mapped
    int fd;            // fd kept open while mapped (>=0) or -1 for shm open only
    bool is_shm;       // mapped from /cxlmemsim_shared
    size_t data_off;   // data offset inside mapping (for shm skip header)
} map_handle_t;

static inline int open_dax(const char *path) { return open(path, O_RDWR | O_CLOEXEC); }

// Map either a DAX char device or the CXLMemSim shared memory, returning data pointer
// If path is "shm", it maps "/cxlmemsim_shared" and skips a small header (56 bytes).
static inline void *map_region(const char *path, size_t *io_size, size_t offset, map_handle_t *out) {
    memset(out, 0, sizeof(*out));
    out->fd = -1;
    out->is_shm = false;
    out->data_off = 0;

    if (strcmp(path, "shm") == 0) {
        // CXLMemSim fallback
        const char *shm_name = "/cxlmemsim_shared";
        int fd = shm_open(shm_name, O_RDWR, 0666);
        if (fd < 0) {
            perror("shm_open /cxlmemsim_shared");
            return NULL;
        }
        struct stat st;
        if (fstat(fd, &st) != 0) {
            perror("fstat shm");
            close(fd);
            return NULL;
        }
        size_t total = (size_t)st.st_size;
        void *p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            perror("mmap shm");
            close(fd);
            return NULL;
        }
        out->base = p;
        out->map_size = total;
        out->fd = fd;
        out->is_shm = true;
        out->data_off = 56; // typical shim header used elsewhere in repo
        if (offset < out->data_off) offset = out->data_off; // ensure we skip header
        if (*io_size == 0 || offset + *io_size > total) {
            *io_size = (total > offset) ? (total - offset) : 0;
        }
        return (char *)p + offset;
    }

    int fd = open_dax(path);
    if (fd < 0) {
        perror("open dax");
        return NULL;
    }

    size_t size = read_sysfs_dax_size(path);
    if (size == 0) {
        // Fallback to a conservative default
        size = 256UL * 1024 * 1024;
    }
    if (*io_size == 0 || offset + *io_size > size) {
        *io_size = (size > offset) ? (size - offset) : 0;
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap dax");
        close(fd);
        return NULL;
    }
    out->base = p;
    out->map_size = size;
    out->fd = fd;
    return (char *)p + offset;
}

static inline void unmap_region(map_handle_t *h) {
    if (h->base && h->map_size) munmap(h->base, h->map_size);
    if (h->fd >= 0) close(h->fd);
    memset(h, 0, sizeof(*h));
}

static inline role_t parse_role(const char *s) {
    if (s == NULL) return ROLE_A;
    if (s[0] == 'A' || s[0] == 'a') return ROLE_A;
    return ROLE_B;
}

static inline void busy_pause(void) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000}; // 1ms
    nanosleep(&ts, NULL);
}

static inline void memset_slow(volatile uint8_t *p, uint8_t v, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = v;
}

