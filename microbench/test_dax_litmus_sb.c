// Store Buffering (SB) litmus adapted for two hosts via shared DAX
// A: x=1; r1=load y
// B: y=1; r2=load x
// Forbidden on x86/TSO: r1==0 && r2==0

#include "dax_litmus_common.h"

typedef struct {
    _Atomic uint32_t x;
    _Atomic uint32_t y;
    _Atomic uint32_t r1;
    _Atomic uint32_t r2;
    _Atomic uint32_t done_a;
    _Atomic uint32_t done_b;
} __attribute__((aligned(CACHELINE_SIZE))) sb_area_t;

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s <role:A|B> <path:/dev/daxX.Y|shm> [iters] [offset_bytes]\n",
            argv0);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }
    role_t role = parse_role(argv[1]);
    const char *path = argv[2];
    uint32_t iters = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 10000u;
    size_t offset = (argc > 4) ? strtoull(argv[4], NULL, 0) : 0ULL;

    size_t size = 16 * 1024 * 1024;
    map_handle_t mh;
    void *region = map_region(path, &size, offset, &mh);
    if (!region || size < 8192) {
        fprintf(stderr, "Failed to map region or size too small\n");
        return 2;
    }

    ctrl_block_t *ctrl = (ctrl_block_t *)region;
    sb_area_t *sb = (sb_area_t *)((uint8_t *)region + 4096);

    // Handshake
    if (role == ROLE_A) {
        atomic_store_explicit(&ctrl->ready_a, 1, memory_order_release);
        while (atomic_load_explicit(&ctrl->ready_b, memory_order_acquire) == 0) busy_pause();
        atomic_store_explicit(&ctrl->magic, 0x51B51B51u, memory_order_release);
    } else {
        atomic_store_explicit(&ctrl->ready_b, 1, memory_order_release);
        while (atomic_load_explicit(&ctrl->ready_a, memory_order_acquire) == 0) busy_pause();
        while (atomic_load_explicit(&ctrl->magic, memory_order_acquire) != 0x51B51B51u) busy_pause();
    }

    uint64_t forbidden = 0, total = 0;
    for (uint32_t iter = 1; iter <= iters; iter++) {
        if (role == ROLE_A) {
            atomic_store_explicit(&sb->x, 0, memory_order_relaxed);
            atomic_store_explicit(&sb->y, 0, memory_order_relaxed);
            atomic_store_explicit(&sb->r1, 2, memory_order_relaxed);
            atomic_store_explicit(&sb->r2, 2, memory_order_relaxed);
            atomic_store_explicit(&sb->done_a, 0, memory_order_relaxed);
            atomic_store_explicit(&sb->done_b, 0, memory_order_relaxed);
            atomic_thread_fence(memory_order_seq_cst);
            atomic_store_explicit(&ctrl->seq, iter, memory_order_release);
        } else {
            while (atomic_load_explicit(&ctrl->seq, memory_order_acquire) != iter) busy_pause();
        }

        // Critical section
        if (role == ROLE_A) {
            atomic_store_explicit(&sb->x, 1, memory_order_release);
            uint32_t r1 = atomic_load_explicit(&sb->y, memory_order_acquire);
            atomic_store_explicit(&sb->r1, r1, memory_order_release);
            atomic_store_explicit(&sb->done_a, 1, memory_order_release);
        } else {
            atomic_store_explicit(&sb->y, 1, memory_order_release);
            uint32_t r2 = atomic_load_explicit(&sb->x, memory_order_acquire);
            atomic_store_explicit(&sb->r2, r2, memory_order_release);
            atomic_store_explicit(&sb->done_b, 1, memory_order_release);
        }

        // Join and check
        if (role == ROLE_A) {
            while (atomic_load_explicit(&sb->done_b, memory_order_acquire) == 0) busy_pause();
            uint32_t r1 = atomic_load_explicit(&sb->r1, memory_order_acquire);
            uint32_t r2 = atomic_load_explicit(&sb->r2, memory_order_acquire);
            total++;
            if (r1 == 0 && r2 == 0) forbidden++;
        } else {
            while (atomic_load_explicit(&sb->done_a, memory_order_acquire) == 0) busy_pause();
        }
    }

    if (role == ROLE_A) {
        printf("[SB] total=%" PRIu64 ", forbidden(r1==0&&r2==0)=%" PRIu64 "\n", total, forbidden);
        // On x86/TSO this should be zero. Any non-zero suggests ordering/coherency violation.
        return (forbidden == 0) ? 0 : 4;
    }

    unmap_region(&mh);
    return 0;
}

