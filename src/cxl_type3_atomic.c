#include "cxl_type3_atomic.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ATOMIC_BAR_SIZE 0x1000U
#define ATOMIC_MAGIC 0x54415843U
#define ATOMIC_VERSION 1U

#define REG_MAGIC 0x00
#define REG_VERSION 0x04
#define REG_CAPS 0x08
#define REG_STATUS 0x0c
#define REG_OP 0x10
#define REG_SERVER_STATUS 0x14
#define REG_ADDR 0x18
#define REG_VALUE 0x20
#define REG_EXPECTED 0x28
#define REG_OLD_VALUE 0x30
#define REG_DOORBELL 0x38

#define CAP_FAA (1U << 0)
#define CAP_CAS (1U << 1)
#define CAP_FENCE (1U << 2)

#define STATUS_IDLE 0U
#define STATUS_BUSY 1U
#define STATUS_DONE 2U
#define STATUS_ERROR 3U

#define OP_FAA 3U
#define OP_CAS 4U
#define OP_FENCE 5U

struct cxl_type3_atomic {
    int fd;
    volatile uint8_t *bar;
    atomic_flag thread_lock;
    uint32_t caps;
    uint32_t last_server_status;
};

struct cxl_type3_atomic_auto_state {
    cxl_type3_atomic_t *ctx;
    uintptr_t mapping_start;
    uintptr_t mapping_end;
    uint64_t dpa_base;
    bool caller_owned;
};

static struct cxl_type3_atomic_auto_state auto_state;
static atomic_flag auto_state_lock = ATOMIC_FLAG_INIT;

static void lock_auto_state(void) {
    while (atomic_flag_test_and_set_explicit(&auto_state_lock, memory_order_acquire)) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
}

static void unlock_auto_state(void) { atomic_flag_clear_explicit(&auto_state_lock, memory_order_release); }

static inline uint32_t mmio_read32(cxl_type3_atomic_t *ctx, size_t offset) {
    uint32_t value = *(volatile uint32_t *)(ctx->bar + offset);
    atomic_signal_fence(memory_order_seq_cst);
    return value;
}

static inline uint64_t mmio_read64(cxl_type3_atomic_t *ctx, size_t offset) {
    uint64_t value = *(volatile uint64_t *)(ctx->bar + offset);
    atomic_signal_fence(memory_order_seq_cst);
    return value;
}

static inline void mmio_write32(cxl_type3_atomic_t *ctx, size_t offset, uint32_t value) {
    atomic_signal_fence(memory_order_seq_cst);
    *(volatile uint32_t *)(ctx->bar + offset) = value;
    atomic_signal_fence(memory_order_seq_cst);
}

static inline void mmio_write64(cxl_type3_atomic_t *ctx, size_t offset, uint64_t value) {
    atomic_signal_fence(memory_order_seq_cst);
    *(volatile uint64_t *)(ctx->bar + offset) = value;
    atomic_signal_fence(memory_order_seq_cst);
}

static int lock_context(cxl_type3_atomic_t *ctx, bool *file_locked) {
    while (atomic_flag_test_and_set_explicit(&ctx->thread_lock, memory_order_acquire)) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }

    *file_locked = false;
    while (flock(ctx->fd, LOCK_EX) < 0) {
        if (errno == EINTR) {
            continue;
        }
        int saved_errno = errno;
        atomic_flag_clear_explicit(&ctx->thread_lock, memory_order_release);
        return -saved_errno;
    }
    *file_locked = true;
    return 0;
}

static void unlock_context(cxl_type3_atomic_t *ctx, bool file_locked) {
    if (file_locked) {
        (void)flock(ctx->fd, LOCK_UN);
    }
    atomic_flag_clear_explicit(&ctx->thread_lock, memory_order_release);
}

static int wait_for_completion(cxl_type3_atomic_t *ctx) {
    const int timeout_ms = 5000;
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};

    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        uint32_t status = mmio_read32(ctx, REG_STATUS);

        if (status == STATUS_DONE) {
            ctx->last_server_status = mmio_read32(ctx, REG_SERVER_STATUS);
            return 0;
        }
        if (status == STATUS_ERROR) {
            ctx->last_server_status = mmio_read32(ctx, REG_SERVER_STATUS);
            return -EREMOTEIO;
        }
        nanosleep(&delay, NULL);
    }
    return -ETIMEDOUT;
}

static int reset_command(cxl_type3_atomic_t *ctx) {
    const int timeout_ms = 5000;
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};

    mmio_write32(ctx, REG_DOORBELL, 0);
    for (int elapsed = 0; elapsed < timeout_ms; elapsed++) {
        if (mmio_read32(ctx, REG_STATUS) == STATUS_IDLE) {
            return 0;
        }
        nanosleep(&delay, NULL);
    }
    return -ETIMEDOUT;
}

static int issue_command(cxl_type3_atomic_t *ctx, uint32_t op, uint64_t dpa, uint64_t value, uint64_t expected,
                         uint64_t *old_value) {
    bool file_locked;
    int ret;

    if (!ctx || (op != OP_FENCE && (dpa & 7))) {
        return -EINVAL;
    }

    ret = lock_context(ctx, &file_locked);
    if (ret != 0) {
        return ret;
    }
    ret = reset_command(ctx);
    if (ret != 0) {
        unlock_context(ctx, file_locked);
        return ret;
    }
    mmio_write64(ctx, REG_ADDR, dpa);
    mmio_write64(ctx, REG_VALUE, value);
    mmio_write64(ctx, REG_EXPECTED, expected);
    mmio_write32(ctx, REG_OP, op);
    atomic_thread_fence(memory_order_seq_cst);
    mmio_write32(ctx, REG_DOORBELL, 1);

    ret = wait_for_completion(ctx);
    if (ret == 0 && old_value) {
        *old_value = mmio_read64(ctx, REG_OLD_VALUE);
    }
    atomic_thread_fence(memory_order_seq_cst);
    unlock_context(ctx, file_locked);
    return ret;
}

int cxl_type3_atomic_open_resource(const char *resource_path, cxl_type3_atomic_t **out) {
    cxl_type3_atomic_t *ctx;
    void *mapping;
    int fd;

    if (!resource_path || !out) {
        return -EINVAL;
    }
    *out = NULL;

    fd = open(resource_path, O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    mapping = mmap(NULL, ATOMIC_BAR_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        int saved_errno = errno;
        close(fd);
        return -saved_errno;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        munmap(mapping, ATOMIC_BAR_SIZE);
        close(fd);
        return -ENOMEM;
    }
    ctx->fd = fd;
    ctx->bar = mapping;
    atomic_flag_clear(&ctx->thread_lock);

    if (mmio_read32(ctx, REG_MAGIC) != ATOMIC_MAGIC || mmio_read32(ctx, REG_VERSION) != ATOMIC_VERSION) {
        cxl_type3_atomic_close(ctx);
        return -ENODEV;
    }
    ctx->caps = mmio_read32(ctx, REG_CAPS);
    *out = ctx;
    return 0;
}

int cxl_type3_atomic_open_bdf(const char *bdf, cxl_type3_atomic_t **out) {
    char path[PATH_MAX];
    int len;

    if (!bdf || strchr(bdf, '/')) {
        return -EINVAL;
    }
    len = snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource5", bdf);
    if (len < 0 || (size_t)len >= sizeof(path)) {
        return -ENAMETOOLONG;
    }
    return cxl_type3_atomic_open_resource(path, out);
}

static bool sysfs_value_matches(const char *device_path, const char *name, unsigned expected) {
    char path[PATH_MAX];
    int len;
    unsigned value;
    FILE *stream;

    len = snprintf(path, sizeof(path), "%s/%s", device_path, name);
    if (len < 0 || (size_t)len >= sizeof(path)) {
        return false;
    }
    stream = fopen(path, "r");
    if (!stream) {
        return false;
    }
    bool matched = fscanf(stream, "%x", &value) == 1 && value == expected;
    fclose(stream);
    return matched;
}

int cxl_type3_atomic_open_auto(cxl_type3_atomic_t **out) {
    const char *resource = getenv("CXL_TYPE3_ATOMIC_RESOURCE");
    const char *bdf = getenv("CXL_TYPE3_ATOMIC_BDF");
    const char *devices = "/sys/bus/pci/devices";
    struct dirent *entry;
    DIR *dir;
    int ret = -ENODEV;

    if (!out) {
        return -EINVAL;
    }
    *out = NULL;

    if (resource && resource[0]) {
        return cxl_type3_atomic_open_resource(resource, out);
    }
    if (bdf && bdf[0]) {
        return cxl_type3_atomic_open_bdf(bdf, out);
    }

    dir = opendir(devices);
    if (!dir) {
        return -errno;
    }
    while ((entry = readdir(dir)) != NULL) {
        char device_path[PATH_MAX];
        char resource_path[PATH_MAX];
        int len;

        if (entry->d_name[0] == '.') {
            continue;
        }
        len = snprintf(device_path, sizeof(device_path), "%s/%s", devices, entry->d_name);
        if (len < 0 || (size_t)len >= sizeof(device_path) || !sysfs_value_matches(device_path, "vendor", 0x8086) ||
            !sysfs_value_matches(device_path, "device", 0x0d93)) {
            continue;
        }
        len = snprintf(resource_path, sizeof(resource_path), "%s/resource5", device_path);
        if (len < 0 || (size_t)len >= sizeof(resource_path)) {
            continue;
        }
        ret = cxl_type3_atomic_open_resource(resource_path, out);
        if (ret == 0) {
            break;
        }
    }
    closedir(dir);
    return ret;
}

void cxl_type3_atomic_close(cxl_type3_atomic_t *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->bar && ctx->bar != MAP_FAILED) {
        munmap((void *)ctx->bar, ATOMIC_BAR_SIZE);
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
    free(ctx);
}

int cxl_type3_atomic_fetch_add_u64(cxl_type3_atomic_t *ctx, uint64_t dpa, uint64_t value, uint64_t *old_value) {
    if (!ctx) {
        return -EINVAL;
    }
    if (!(ctx->caps & CAP_FAA)) {
        return -EOPNOTSUPP;
    }
    return issue_command(ctx, OP_FAA, dpa, value, 0, old_value);
}

int cxl_type3_atomic_compare_exchange_u64(cxl_type3_atomic_t *ctx, uint64_t dpa, uint64_t expected, uint64_t desired,
                                          uint64_t *old_value) {
    if (!ctx) {
        return -EINVAL;
    }
    if (!(ctx->caps & CAP_CAS)) {
        return -EOPNOTSUPP;
    }
    return issue_command(ctx, OP_CAS, dpa, desired, expected, old_value);
}

int cxl_type3_atomic_fence(cxl_type3_atomic_t *ctx) {
    if (!ctx) {
        return -EINVAL;
    }
    if (!(ctx->caps & CAP_FENCE)) {
        return -EOPNOTSUPP;
    }
    return issue_command(ctx, OP_FENCE, 0, 0, 0, NULL);
}

uint32_t cxl_type3_atomic_last_server_status(const cxl_type3_atomic_t *ctx) {
    return ctx ? ctx->last_server_status : UINT32_MAX;
}

static int compiler_memory_order(int order) {
    switch (order) {
    case memory_order_relaxed:
        return __ATOMIC_RELAXED;
    case memory_order_consume:
        return __ATOMIC_CONSUME;
    case memory_order_acquire:
        return __ATOMIC_ACQUIRE;
    case memory_order_release:
        return __ATOMIC_RELEASE;
    case memory_order_acq_rel:
        return __ATOMIC_ACQ_REL;
    case memory_order_seq_cst:
    default:
        return __ATOMIC_SEQ_CST;
    }
}

static uint64_t native_fetch_add(volatile void *object, uint64_t value, int order) {
    return __atomic_fetch_add((volatile uint64_t *)object, value, compiler_memory_order(order));
}

static bool is_dax_path(const char *path) { return path && strncmp(path, "/dev/dax", strlen("/dev/dax")) == 0; }

static int configured_dpa_base(uint64_t *base) {
    const char *text = getenv("CXL_TYPE3_ATOMIC_DPA_BASE");
    char *end;
    unsigned long long value;

    *base = 0;
    if (!text || !text[0]) {
        return 0;
    }
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return -EINVAL;
    }
    *base = (uint64_t)value;
    return 0;
}

static int discover_dax_mapping(uintptr_t address, uintptr_t *mapping_start, uintptr_t *mapping_end,
                                uint64_t *dpa_base) {
    FILE *maps;
    char line[PATH_MAX + 128];
    int ret = 0;

    maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        return -errno;
    }

    while (fgets(line, sizeof(line), maps)) {
        unsigned long long start;
        unsigned long long end;
        unsigned long long file_offset;
        char permissions[5];
        char path[PATH_MAX] = {0};
        int fields;

        fields = sscanf(line, "%llx-%llx %4s %llx %*s %*s %4095s", &start, &end, permissions, &file_offset, path);
        if (fields < 4 || address < start || address >= end) {
            continue;
        }
        if (fields < 5 || !is_dax_path(path)) {
            ret = 0;
            break;
        }

        uint64_t configured_base;
        ret = configured_dpa_base(&configured_base);
        if (ret != 0) {
            break;
        }
        if (file_offset > UINT64_MAX - configured_base) {
            ret = -EOVERFLOW;
            break;
        }
        *mapping_start = (uintptr_t)start;
        *mapping_end = (uintptr_t)end;
        *dpa_base = configured_base + (uint64_t)file_offset;
        ret = 1;
        break;
    }

    fclose(maps);
    return ret;
}

int cxl_type3_atomic_auto_bind(cxl_type3_atomic_t *ctx, const void *mapping_base, size_t mapping_length,
                               uint64_t dpa_base) {
    uintptr_t start = (uintptr_t)mapping_base;

    if ((ctx == NULL) != (mapping_base == NULL) || (ctx && mapping_length == 0) ||
        (ctx && start > UINTPTR_MAX - mapping_length)) {
        return -EINVAL;
    }

    lock_auto_state();
    if (auto_state.ctx && !auto_state.caller_owned) {
        cxl_type3_atomic_close(auto_state.ctx);
    }
    memset(&auto_state, 0, sizeof(auto_state));
    if (ctx) {
        auto_state.ctx = ctx;
        auto_state.mapping_start = start;
        auto_state.mapping_end = start + mapping_length;
        auto_state.dpa_base = dpa_base;
        auto_state.caller_owned = true;
    }
    unlock_auto_state();
    return 0;
}

uint64_t cxl_type3_atomic_fetch_add_auto_u64(volatile void *object, uint64_t value, int order) {
    uintptr_t address = (uintptr_t)object;
    uint64_t mapping_offset;
    uint64_t dpa;
    uint64_t old_value;
    int ret;

    if (!object || (address & (sizeof(uint64_t) - 1))) {
        fprintf(stderr, "CXLMemSim automatic atomic: invalid or unaligned object %p\n", (const void *)object);
        abort();
    }

    lock_auto_state();
    if (auto_state.ctx && address >= auto_state.mapping_start && address < auto_state.mapping_end) {
        mapping_offset = (uint64_t)(address - auto_state.mapping_start);
        if (auto_state.mapping_end - address < sizeof(uint64_t) || mapping_offset > UINT64_MAX - auto_state.dpa_base) {
            unlock_auto_state();
            fprintf(stderr, "CXLMemSim automatic atomic: object %p crosses its mapping or DPA overflows\n",
                    (const void *)object);
            abort();
        }
        dpa = auto_state.dpa_base + mapping_offset;
    } else if (auto_state.caller_owned) {
        unlock_auto_state();
        return native_fetch_add(object, value, order);
    } else {
        uintptr_t mapping_start;
        uintptr_t mapping_end;
        uint64_t mapping_dpa_base;

        ret = discover_dax_mapping(address, &mapping_start, &mapping_end, &mapping_dpa_base);
        if (ret == 0) {
            unlock_auto_state();
            return native_fetch_add(object, value, order);
        }
        if (ret < 0) {
            unlock_auto_state();
            fprintf(stderr, "CXLMemSim automatic atomic: cannot resolve DAX mapping for %p: %s\n", (const void *)object,
                    strerror(-ret));
            abort();
        }

        if (!auto_state.ctx) {
            ret = cxl_type3_atomic_open_auto(&auto_state.ctx);
            if (ret != 0) {
                unlock_auto_state();
                fprintf(stderr, "CXLMemSim automatic atomic: DAX pointer %p has no usable atomic BAR: %s\n",
                        (const void *)object, strerror(-ret));
                abort();
            }
        }
        auto_state.mapping_start = mapping_start;
        auto_state.mapping_end = mapping_end;
        auto_state.dpa_base = mapping_dpa_base;
        mapping_offset = (uint64_t)(address - mapping_start);
        if (mapping_end - address < sizeof(uint64_t) || mapping_offset > UINT64_MAX - mapping_dpa_base) {
            unlock_auto_state();
            fprintf(stderr, "CXLMemSim automatic atomic: object %p crosses its mapping or DPA overflows\n",
                    (const void *)object);
            abort();
        }
        dpa = mapping_dpa_base + mapping_offset;
    }

    ret = cxl_type3_atomic_fetch_add_u64(auto_state.ctx, dpa, value, &old_value);
    if (ret != 0) {
        uint32_t server_status = cxl_type3_atomic_last_server_status(auto_state.ctx);
        unlock_auto_state();
        fprintf(stderr, "CXLMemSim automatic atomic: FAA at DPA 0x%" PRIx64 " failed: %s (server status=%u)\n", dpa,
                strerror(-ret), server_status);
        abort();
    }
    unlock_auto_state();
    return old_value;
}
