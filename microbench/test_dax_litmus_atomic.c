// Cross-host atomic RMW: both sides do fetch_add on shared counter

#include "dax_litmus_common.h"

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s <role:A|B> <path:/dev/daxX.Y|shm> [adds] [offset_bytes]\n", argv0);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }
    role_t role = parse_role(argv[1]);
    const char *path = argv[2];
    uint64_t adds = (argc > 3) ? strtoull(argv[3], NULL, 0) : 1000000ULL;
    size_t offset = (argc > 4) ? strtoull(argv[4], NULL, 0) : 0ULL;

    size_t size = 4 * 1024 * 1024;
    map_handle_t mh;
    void *region = map_region(path, &size, offset, &mh);
    if (!region || size < 4096) {
        fprintf(stderr, "Failed to map region\n");
        return 2;
    }
    ctrl_block_t *ctrl = (ctrl_block_t *)region;

    // Init & handshake
    if (role == ROLE_A) {
        atomic_store_explicit(&ctrl->counter, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->ready_a, 1, memory_order_release);
        while (atomic_load_explicit(&ctrl->ready_b, memory_order_acquire) == 0) busy_pause();
        atomic_store_explicit(&ctrl->magic, 0xA71A71A7u, memory_order_release);
    } else {
        atomic_store_explicit(&ctrl->ready_b, 1, memory_order_release);
        while (atomic_load_explicit(&ctrl->ready_a, memory_order_acquire) == 0) busy_pause();
        while (atomic_load_explicit(&ctrl->magic, memory_order_acquire) != 0xA71A71A7u) busy_pause();
    }

    // Start signal
    if (role == ROLE_A) {
        atomic_store_explicit(&ctrl->seq, 1, memory_order_release);
    } else {
        while (atomic_load_explicit(&ctrl->seq, memory_order_acquire) != 1) busy_pause();
    }

    // RMW loop
    for (uint64_t i = 0; i < adds; i++) {
        (void)atomic_fetch_add_explicit(&ctrl->counter, 1, memory_order_acq_rel);
    }

    // Signal done
    if (role == ROLE_A) {
        atomic_store_explicit(&ctrl->flag, 1, memory_order_release);
        while (atomic_load_explicit(&ctrl->ready_b, memory_order_acquire) != 2) busy_pause();
        uint64_t v = atomic_load_explicit(&ctrl->counter, memory_order_acquire);
        printf("[ATOMIC] final=%" PRIu64 " expected=%" PRIu64 "\n", v, adds * 2ULL);
        return (v == adds * 2ULL) ? 0 : 5;
    } else {
        // reuse ready_b as done flag to keep ctrl compact
        atomic_store_explicit(&ctrl->ready_b, 2, memory_order_release);
        while (atomic_load_explicit(&ctrl->flag, memory_order_acquire) != 1) busy_pause();
    }

    unmap_region(&mh);
    return 0;
}

