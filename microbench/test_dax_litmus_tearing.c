// Tearing check: paired 64-bit values written atomically must read coherently
// Writer publishes (v, ~v) with release; reader acquires and checks v ^ ~v == ~0

#include "dax_litmus_common.h"

typedef struct {
    _Atomic uint64_t v;
    _Atomic uint64_t v_bar;
} __attribute__((aligned(CACHELINE_SIZE))) pair_t;

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s <role:A|B> <path:/dev/daxX.Y|shm> [iters] [offset_bytes]\n", argv0);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }
    role_t role = parse_role(argv[1]);
    const char *path = argv[2];
    uint64_t iters = (argc > 3) ? strtoull(argv[3], NULL, 0) : 2000000ULL;
    size_t offset = (argc > 4) ? strtoull(argv[4], NULL, 0) : 0ULL;

    size_t size = 4 * 1024 * 1024;
    map_handle_t mh;
    void *region = map_region(path, &size, offset, &mh);
    if (!region) {
        fprintf(stderr, "Failed to map region\n");
        return 2;
    }
    ctrl_block_t *ctrl = (ctrl_block_t *)region;
    pair_t *p = (pair_t *)((uint8_t *)region + 4096);

    // Handshake
    if (role == ROLE_A) {
        atomic_store_explicit(&ctrl->ready_a, 1, memory_order_release);
        while (atomic_load_explicit(&ctrl->ready_b, memory_order_acquire) == 0) busy_pause();
        atomic_store_explicit(&ctrl->magic, 0x7EA71234u, memory_order_release);
        atomic_store_explicit(&p->v, 0, memory_order_relaxed);
        atomic_store_explicit(&p->v_bar, ~0ULL, memory_order_relaxed);
    } else {
        atomic_store_explicit(&ctrl->ready_b, 1, memory_order_release);
        while (atomic_load_explicit(&ctrl->ready_a, memory_order_acquire) == 0) busy_pause();
        while (atomic_load_explicit(&ctrl->magic, memory_order_acquire) != 0x7EA71234u) busy_pause();
    }

    // Start signal
    if (role == ROLE_A) atomic_store_explicit(&ctrl->seq, 1, memory_order_release);
    else while (atomic_load_explicit(&ctrl->seq, memory_order_acquire) != 1) busy_pause();

    uint64_t errs = 0, reads = 0;
    if (role == ROLE_A) {
        for (uint64_t s = 1; s <= iters; s++) {
            atomic_store_explicit(&p->v, s, memory_order_relaxed);
            atomic_store_explicit(&p->v_bar, ~s, memory_order_release);
        }
        // Tell reader done
        atomic_store_explicit(&ctrl->flag, 1, memory_order_release);
    } else {
        while (atomic_load_explicit(&ctrl->flag, memory_order_acquire) == 0) {
            uint64_t vbar = atomic_load_explicit(&p->v_bar, memory_order_acquire);
            uint64_t v = atomic_load_explicit(&p->v, memory_order_relaxed);
            reads++;
            if ((v ^ vbar) != ~0ULL) {
                errs++;
            }
        }
        printf("[TEAR] reads=%" PRIu64 " errs=%" PRIu64 "\n", reads, errs);
        // Any non-zero errs indicates torn/incoherent read across 64-bit pair
        return (errs == 0) ? 0 : 6;
    }

    unmap_region(&mh);
    return 0;
}

