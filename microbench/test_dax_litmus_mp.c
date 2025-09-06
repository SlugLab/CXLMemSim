// Message-passing litmus: A writes data then sets flag; B observes
// Validates write->flag ordering across two hosts sharing a DAX mapping

#include "dax_litmus_common.h"

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s <role:A|B> <path:/dev/daxX.Y|shm> [size_MB] [offset_bytes]\n",
            argv0);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }
    role_t role = parse_role(argv[1]);
    const char *path = argv[2];
    size_t size = (argc > 3) ? (strtoull(argv[3], NULL, 0) * 1024ULL * 1024ULL) : 16ULL * 1024 * 1024;
    size_t offset = (argc > 4) ? strtoull(argv[4], NULL, 0) : 0ULL;

    map_handle_t mh;
    void *region = map_region(path, &size, offset, &mh);
    if (!region || size < 8192) {
        fprintf(stderr, "Failed to map region or size too small\n");
        return 2;
    }

    ctrl_block_t *ctrl = (ctrl_block_t *)region;                   // control at base
    uint8_t *payload = (uint8_t *)region + 4096;                    // data after a page
    size_t payload_len = (size > 8192) ? (size - 8192) : 4096;      // at least 4KB
    if (payload_len > (256 * 1024)) payload_len = 256 * 1024;       // cap to keep runs quick

    // Clear control on first runner to known state if both zero
    if (role == ROLE_A) {
        uint32_t ra = atomic_load_explicit(&ctrl->ready_a, memory_order_relaxed);
        uint32_t rb = atomic_load_explicit(&ctrl->ready_b, memory_order_relaxed);
        if (ra == 0 && rb == 0) {
            atomic_store_explicit(&ctrl->magic, 0, memory_order_relaxed);
            atomic_store_explicit(&ctrl->seq, 0, memory_order_relaxed);
            atomic_store_explicit(&ctrl->flag, 0, memory_order_relaxed);
        }
        atomic_store_explicit(&ctrl->ready_a, 1, memory_order_release);
        // Wait for B with timeout and debug prints
        uint64_t spins = 0;
        while (atomic_load_explicit(&ctrl->ready_b, memory_order_acquire) == 0) {
            if ((++spins % 1000) == 0) {
                fprintf(stderr, "[MP] waiting ready_b... ra=%u rb=%u magic=%u\n",
                        atomic_load_explicit(&ctrl->ready_a, memory_order_relaxed),
                        atomic_load_explicit(&ctrl->ready_b, memory_order_relaxed),
                        atomic_load_explicit(&ctrl->magic, memory_order_relaxed));
            }
            busy_pause();
            if (spins > 60000) { // ~60s
                fprintf(stderr, "[MP] timeout waiting for ready_b. Check shared backend/offset.\n");
                unmap_region(&mh);
                return 10;
            }
        }
        atomic_store_explicit(&ctrl->magic, 0xC0DEC0DEu, memory_order_release);
    } else {
        atomic_store_explicit(&ctrl->ready_b, 1, memory_order_release);
        uint64_t spins = 0;
        while (atomic_load_explicit(&ctrl->ready_a, memory_order_acquire) == 0) {
            if ((++spins % 1000) == 0) {
                fprintf(stderr, "[MP] waiting ready_a... ra=%u rb=%u magic=%u\n",
                        atomic_load_explicit(&ctrl->ready_a, memory_order_relaxed),
                        atomic_load_explicit(&ctrl->ready_b, memory_order_relaxed),
                        atomic_load_explicit(&ctrl->magic, memory_order_relaxed));
            }
            busy_pause();
            if (spins > 60000) {
                fprintf(stderr, "[MP] timeout waiting for ready_a. Check shared backend/offset.\n");
                unmap_region(&mh);
                return 11;
            }
        }
        spins = 0;
        while (atomic_load_explicit(&ctrl->magic, memory_order_acquire) != 0xC0DEC0DEu) {
            if ((++spins % 1000) == 0) {
                fprintf(stderr, "[MP] waiting magic... ra=%u rb=%u magic=%u\n",
                        atomic_load_explicit(&ctrl->ready_a, memory_order_relaxed),
                        atomic_load_explicit(&ctrl->ready_b, memory_order_relaxed),
                        atomic_load_explicit(&ctrl->magic, memory_order_relaxed));
            }
            busy_pause();
            if (spins > 60000) {
                fprintf(stderr, "[MP] timeout waiting for magic. Check A is running.\n");
                unmap_region(&mh);
                return 12;
            }
        }
    }

    const uint32_t iters = 1000;
    uint32_t fails = 0;
    if (role == ROLE_A) {
        for (uint32_t s = 1; s <= iters; s++) {
            // fill payload with a simple byte pattern
            uint8_t v = (uint8_t)(s & 0xFF);
            memset_slow(payload, v, payload_len);
            atomic_thread_fence(memory_order_release);
            atomic_store_explicit(&ctrl->seq, s, memory_order_relaxed);
            atomic_store_explicit(&ctrl->flag, s, memory_order_release);
            // Wait for B to observe and clear
            while (atomic_load_explicit(&ctrl->flag, memory_order_acquire) != 0) busy_pause();
        }
    } else {
        uint32_t expect = 1;
        while (expect <= iters) {
            uint32_t f = atomic_load_explicit(&ctrl->flag, memory_order_acquire);
            if (f == expect) {
                uint32_t seq = atomic_load_explicit(&ctrl->seq, memory_order_relaxed);
                atomic_thread_fence(memory_order_acquire);
                if (seq != expect) {
                    fprintf(stderr, "Seq mismatch: flag=%u seq=%u\n", f, seq);
                    fails++;
                }
                // verify payload
                uint8_t expected = (uint8_t)(expect & 0xFF);
                for (size_t i = 0; i < payload_len; i++) {
                    if (payload[i] != expected) {
                        fails++;
                        fprintf(stderr, "Payload mismatch at %zu: got %02x exp %02x (iter %u)\n",
                                i, payload[i], expected, expect);
                        break;
                    }
                }
                // Ack to A
                atomic_store_explicit(&ctrl->flag, 0, memory_order_release);
                expect++;
            } else {
                busy_pause();
            }
        }
    }

    if (role == ROLE_A) {
        // Tell B we’re done
        atomic_store_explicit(&ctrl->seq, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->flag, 0, memory_order_release);
    }

    unmap_region(&mh);
    if (fails == 0) {
        printf("[MP] PASS (%u iterations, payload %zu bytes)\n", iters, payload_len);
        return 0;
    } else {
        printf("[MP] FAIL with %u errors\n", fails);
        return 3;
    }
}
