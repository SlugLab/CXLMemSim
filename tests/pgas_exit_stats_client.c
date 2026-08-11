#define _POSIX_C_SOURCE 200809L

#include "cxl_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ns(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int wait_for_u32(volatile uint32_t *value, uint32_t expected, uint64_t timeout_ns) {
    const uint64_t deadline = monotonic_ns() + timeout_ns;
    const struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = 1000000};

    while (__atomic_load_n(value, __ATOMIC_ACQUIRE) != expected) {
        if (monotonic_ns() >= deadline) {
            return -1;
        }
        nanosleep(&sleep_time, NULL);
    }
    return 0;
}

static uint32_t wait_for_response(cxl_shm_slot_t *slot, uint64_t timeout_ns) {
    const uint64_t deadline = monotonic_ns() + timeout_ns;
    const struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = 1000000};

    while (monotonic_ns() < deadline) {
        uint32_t status = __atomic_load_n(&slot->resp_status, __ATOMIC_ACQUIRE);
        if (status != CXL_SHM_RESP_NONE) {
            return status;
        }
        nanosleep(&sleep_time, NULL);
    }
    return CXL_SHM_RESP_NONE;
}

static int submit(cxl_shm_slot_t *slot, uint32_t request, uint64_t address, const void *data, size_t size,
                  uint64_t value, uint64_t expected, uint64_t *returned_value) {
    __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
    slot->addr = address;
    slot->size = size;
    slot->value = value;
    slot->expected = expected;
    slot->latency_ns = 0;
    slot->timestamp = monotonic_ns();
    memset(slot->data, 0, sizeof(slot->data));
    if (data != NULL && size != 0) {
        memcpy(slot->data, data, size);
    }

    __atomic_store_n(&slot->req_type, request, __ATOMIC_RELEASE);
    uint32_t status = wait_for_response(slot, 10000000000ULL);
    if (status != CXL_SHM_RESP_OK) {
        fprintf(stderr, "request %u timed out or failed (status=%u)\n", request, status);
        return -1;
    }

    if (returned_value != NULL) {
        memcpy(returned_value, slot->data, sizeof(*returned_value));
    }
    __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s /pgas-shm-name [slots-to-touch]\n", argv[0]);
        return EXIT_FAILURE;
    }

    unsigned long slots_to_touch = 1;
    if (argc == 3) {
        char *end = NULL;
        errno = 0;
        slots_to_touch = strtoul(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' || slots_to_touch == 0) {
            fprintf(stderr, "invalid slots-to-touch: %s\n", argv[2]);
            return EXIT_FAILURE;
        }
    }

    int fd = shm_open(argv[1], O_RDWR | O_CLOEXEC, 0);
    if (fd < 0) {
        perror("shm_open");
        return EXIT_FAILURE;
    }

    struct stat statbuf;
    if (fstat(fd, &statbuf) != 0 || statbuf.st_size < (off_t)CXL_SHM_HEADER_SIZE(1)) {
        perror("fstat");
        close(fd);
        return EXIT_FAILURE;
    }

    size_t mapped_size = (size_t)statbuf.st_size;
    cxl_shm_header_t *header = mmap(NULL, mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (header == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return EXIT_FAILURE;
    }

    int result = EXIT_FAILURE;
    if (header->magic != CXL_SHM_MAGIC || header->version != CXL_SHM_VERSION || header->num_slots == 0 ||
        CXL_SHM_HEADER_SIZE(header->num_slots) > mapped_size) {
        fprintf(stderr, "invalid PGAS header\n");
        goto out;
    }
    if (slots_to_touch > header->num_slots) {
        fprintf(stderr, "requested %lu slots but server exposes %u\n", slots_to_touch, header->num_slots);
        goto out;
    }
    if (wait_for_u32(&header->server_ready, 1, 10000000000ULL) != 0) {
        fprintf(stderr, "server_ready timed out\n");
        goto out;
    }

    cxl_shm_slot_t *slot = &header->slots[0];
    uint64_t value = 7;
    uint64_t returned = 0;

    if (submit(slot, CXL_SHM_REQ_WRITE, 0, &value, sizeof(value), 0, 0, NULL) != 0 ||
        submit(slot, CXL_SHM_REQ_READ, 0, NULL, sizeof(value), 0, 0, &returned) != 0 || returned != 7 ||
        submit(slot, CXL_SHM_REQ_ATOMIC_FAA, 0, NULL, sizeof(value), 1, 0, &returned) != 0 || returned != 7 ||
        submit(slot, CXL_SHM_REQ_ATOMIC_CAS, 0, NULL, sizeof(value), 9, 8, &returned) != 0 || returned != 8 ||
        submit(slot, CXL_SHM_REQ_FENCE, 0, NULL, 0, 0, 0, NULL) != 0) {
        fprintf(stderr, "PGAS operation validation failed (returned=%" PRIu64 ")\n", returned);
        goto out;
    }

    for (unsigned long i = 1; i < slots_to_touch; ++i) {
        uint64_t slot_value = 0;
        if (submit(&header->slots[i], CXL_SHM_REQ_READ, 0, NULL, sizeof(slot_value), 0, 0, &slot_value) != 0 ||
            slot_value != 9) {
            fprintf(stderr, "PGAS slot %lu validation failed (returned=%" PRIu64 ")\n", i, slot_value);
            goto out;
        }
    }

    result = EXIT_SUCCESS;

out:
    munmap(header, mapped_size);
    close(fd);
    return result;
}
