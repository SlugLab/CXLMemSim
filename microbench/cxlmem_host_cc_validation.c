#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define APPROVED_LENGTH (2ULL * 1024ULL * 1024ULL)
#define MINIMUM_LENGTH 8192ULL
#define CACHELINE_SIZE 64ULL
#define LOAD_OFFSET 4096ULL
#define WATCHDOG_NS (60ULL * 1000ULL * 1000ULL * 1000ULL)

typedef enum {
    MODE_INVALID,
    MODE_WARM_LOAD,
    MODE_COLD_LOAD,
    MODE_MESSAGE_PASSING,
    MODE_HANDOFF,
    MODE_FETCH_ADD,
    MODE_CAS,
} benchmark_mode_t;

typedef enum { BACKEND_INVALID, BACKEND_DRAM, BACKEND_CXLMEM } backend_t;

typedef struct {
    benchmark_mode_t mode;
    backend_t backend;
    const char *device;
    size_t offset;
    size_t length;
    int cpu_a;
    int cpu_b;
    uint64_t iterations;
} options_t;

typedef struct {
    _Alignas(CACHELINE_SIZE) _Atomic uint64_t payload;
    uint8_t payload_padding[CACHELINE_SIZE - sizeof(_Atomic uint64_t)];
    _Alignas(CACHELINE_SIZE) _Atomic uint64_t flag;
    uint8_t flag_padding[CACHELINE_SIZE - sizeof(_Atomic uint64_t)];
    _Alignas(CACHELINE_SIZE) _Atomic uint64_t ack;
    uint8_t ack_padding[CACHELINE_SIZE - sizeof(_Atomic uint64_t)];
    _Alignas(CACHELINE_SIZE) _Atomic uint64_t turn;
    uint8_t turn_padding[CACHELINE_SIZE - sizeof(_Atomic uint64_t)];
    _Alignas(CACHELINE_SIZE) _Atomic uint64_t counter;
    uint8_t counter_padding[CACHELINE_SIZE - sizeof(_Atomic uint64_t)];
    _Alignas(CACHELINE_SIZE) _Atomic uint64_t cas_value;
} shared_state_t;

typedef struct {
    shared_state_t *state;
    uint64_t iterations;
    int cpu;
    int role;
    _Atomic int *ready;
    _Atomic int *start;
    _Atomic int *failed;
    uint64_t errors;
    uint64_t attempts;
    uint64_t successes;
    uint64_t failures;
    uint64_t *tickets;
} worker_args_t;

typedef struct {
    uint64_t errors;
    uint64_t stale;
    uint64_t ticket_errors;
    uint64_t cas_errors;
    uint64_t operations;
    uint64_t final_value;
    uint64_t attempts;
    uint64_t successes;
    uint64_t failures;
    uint64_t flushes;
    double average_ns;
    double p50_ns;
    double p95_ns;
    double p99_ns;
    size_t max_written_offset;
} result_t;

static const char *mode_name(benchmark_mode_t mode) {
    switch (mode) {
    case MODE_WARM_LOAD:
        return "warm-load";
    case MODE_COLD_LOAD:
        return "cold-load";
    case MODE_MESSAGE_PASSING:
        return "message-passing";
    case MODE_HANDOFF:
        return "handoff";
    case MODE_FETCH_ADD:
        return "fetch-add";
    case MODE_CAS:
        return "cas";
    default:
        return "invalid";
    }
}

static const char *backend_name(backend_t backend) {
    return backend == BACKEND_DRAM ? "dram" : backend == BACKEND_CXLMEM ? "cxlmem" : "invalid";
}

static benchmark_mode_t parse_mode(const char *value) {
    if (strcmp(value, "warm-load") == 0)
        return MODE_WARM_LOAD;
    if (strcmp(value, "cold-load") == 0)
        return MODE_COLD_LOAD;
    if (strcmp(value, "message-passing") == 0)
        return MODE_MESSAGE_PASSING;
    if (strcmp(value, "handoff") == 0)
        return MODE_HANDOFF;
    if (strcmp(value, "fetch-add") == 0)
        return MODE_FETCH_ADD;
    if (strcmp(value, "cas") == 0)
        return MODE_CAS;
    return MODE_INVALID;
}

static bool parse_u64(const char *text, uint64_t *value) {
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool parse_int(const char *text, int *value) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < 0 || parsed > INT32_MAX)
        return false;
    *value = (int)parsed;
    return true;
}

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s --mode warm-load|cold-load|message-passing|handoff|fetch-add|cas "
            "--backend dram|cxlmem --length BYTES --cpu-a N --cpu-b N --iterations N "
            "[--device /dev/dax0.0] [--offset 0]\n",
            program);
}

static bool parse_options(int argc, char **argv, options_t *options) {
    *options = (options_t){.device = "/dev/dax0.0", .length = APPROVED_LENGTH, .cpu_a = -1, .cpu_b = -1};
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--help") == 0) {
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        if (index + 1 >= argc)
            return false;
        const char *key = argv[index];
        const char *value = argv[++index];
        uint64_t parsed = 0;
        if (strcmp(key, "--mode") == 0) {
            options->mode = parse_mode(value);
        } else if (strcmp(key, "--backend") == 0) {
            options->backend = strcmp(value, "dram") == 0     ? BACKEND_DRAM
                               : strcmp(value, "cxlmem") == 0 ? BACKEND_CXLMEM
                                                              : BACKEND_INVALID;
        } else if (strcmp(key, "--device") == 0) {
            options->device = value;
        } else if (strcmp(key, "--offset") == 0 && parse_u64(value, &parsed) && parsed <= SIZE_MAX) {
            options->offset = (size_t)parsed;
        } else if (strcmp(key, "--length") == 0 && parse_u64(value, &parsed) && parsed <= SIZE_MAX) {
            options->length = (size_t)parsed;
        } else if (strcmp(key, "--cpu-a") == 0 && parse_int(value, &options->cpu_a)) {
        } else if (strcmp(key, "--cpu-b") == 0 && parse_int(value, &options->cpu_b)) {
        } else if (strcmp(key, "--iterations") == 0 && parse_u64(value, &options->iterations)) {
        } else {
            return false;
        }
    }
    if (options->mode == MODE_INVALID || options->backend == BACKEND_INVALID || options->cpu_a < 0 ||
        options->cpu_b < 0 || options->iterations == 0 || options->length < MINIMUM_LENGTH ||
        options->length > APPROVED_LENGTH)
        return false;
    if (options->backend == BACKEND_CXLMEM && options->offset != 0)
        return false;
    return true;
}

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static uint64_t read_tsc(void) {
    unsigned auxiliary;
    _mm_lfence();
    uint64_t value = __rdtscp(&auxiliary);
    _mm_lfence();
    return value;
}

static double calibrate_cycles_per_ns(void) {
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
    uint64_t start_ns = monotonic_ns();
    uint64_t start_tsc = read_tsc();
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
    uint64_t end_tsc = read_tsc();
    uint64_t end_ns = monotonic_ns();
    return (double)(end_tsc - start_tsc) / (double)(end_ns - start_ns);
}

static bool pin_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

static bool spin_until(_Atomic uint64_t *value, uint64_t expected, _Atomic int *failed, uint64_t deadline) {
    uint64_t spins = 0;
    while (atomic_load_explicit(value, memory_order_acquire) != expected) {
        if (atomic_load_explicit(failed, memory_order_relaxed))
            return false;
        _mm_pause();
        if ((++spins & 0xfffffU) == 0 && monotonic_ns() >= deadline) {
            atomic_store_explicit(failed, 1, memory_order_release);
            return false;
        }
    }
    return true;
}

static bool wait_for_start(worker_args_t *args) {
    atomic_fetch_add_explicit(args->ready, 1, memory_order_acq_rel);
    uint64_t deadline = monotonic_ns() + WATCHDOG_NS;
    while (!atomic_load_explicit(args->start, memory_order_acquire)) {
        if (monotonic_ns() >= deadline) {
            atomic_store_explicit(args->failed, 1, memory_order_release);
            return false;
        }
        _mm_pause();
    }
    return true;
}

static void *message_worker(void *opaque) {
    worker_args_t *args = opaque;
    if (!pin_cpu(args->cpu)) {
        args->errors++;
        atomic_store(args->failed, 1);
        return NULL;
    }
    if (!wait_for_start(args))
        return NULL;
    uint64_t deadline = monotonic_ns() + WATCHDOG_NS;
    if (args->role == 0) {
        for (uint64_t sequence = 1; sequence <= args->iterations; ++sequence) {
            atomic_store_explicit(&args->state->payload, sequence, memory_order_relaxed);
            atomic_store_explicit(&args->state->flag, sequence, memory_order_release);
            if (!spin_until(&args->state->ack, sequence, args->failed, deadline)) {
                args->errors++;
                break;
            }
        }
    } else {
        for (uint64_t sequence = 1; sequence <= args->iterations; ++sequence) {
            if (!spin_until(&args->state->flag, sequence, args->failed, deadline)) {
                args->errors++;
                break;
            }
            if (atomic_load_explicit(&args->state->payload, memory_order_relaxed) != sequence)
                args->errors++;
            atomic_store_explicit(&args->state->ack, sequence, memory_order_release);
        }
    }
    return NULL;
}

static void *handoff_worker(void *opaque) {
    worker_args_t *args = opaque;
    if (!pin_cpu(args->cpu)) {
        args->errors++;
        atomic_store(args->failed, 1);
        return NULL;
    }
    if (!wait_for_start(args))
        return NULL;
    uint64_t deadline = monotonic_ns() + WATCHDOG_NS;
    for (uint64_t iteration = 0; iteration < args->iterations; ++iteration) {
        if (!spin_until(&args->state->turn, (uint64_t)args->role, args->failed, deadline)) {
            args->errors++;
            break;
        }
        uint64_t expected = iteration * 2 + (uint64_t)args->role;
        uint64_t observed = atomic_load_explicit(&args->state->payload, memory_order_relaxed);
        if (observed != expected)
            args->errors++;
        atomic_store_explicit(&args->state->payload, observed + 1, memory_order_relaxed);
        atomic_store_explicit(&args->state->turn, (uint64_t)(1 - args->role), memory_order_release);
    }
    return NULL;
}

static void *fetch_add_worker(void *opaque) {
    worker_args_t *args = opaque;
    if (!pin_cpu(args->cpu)) {
        args->errors++;
        atomic_store(args->failed, 1);
        return NULL;
    }
    if (!wait_for_start(args))
        return NULL;
    for (uint64_t index = 0; index < args->iterations; ++index)
        args->tickets[index] = atomic_fetch_add_explicit(&args->state->counter, 1, memory_order_acq_rel);
    return NULL;
}

static void *cas_worker(void *opaque) {
    worker_args_t *args = opaque;
    if (!pin_cpu(args->cpu)) {
        args->errors++;
        atomic_store(args->failed, 1);
        return NULL;
    }
    if (!wait_for_start(args))
        return NULL;
    for (uint64_t index = 0; index < args->iterations; ++index) {
        uint64_t expected = atomic_load_explicit(&args->state->cas_value, memory_order_relaxed);
        for (;;) {
            args->attempts++;
            if (atomic_compare_exchange_weak_explicit(&args->state->cas_value, &expected, expected + 1,
                                                      memory_order_acq_rel, memory_order_relaxed)) {
                args->successes++;
                break;
            }
            args->failures++;
        }
    }
    return NULL;
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static double percentile(const uint64_t *values, uint64_t count, double fraction, double cycles_per_ns) {
    uint64_t index = (uint64_t)ceil(fraction * (double)count) - 1;
    if (index >= count)
        index = count - 1;
    return (double)values[index] / cycles_per_ns;
}

static bool initialize_pointer_cycle(uint8_t *mapping, size_t length, uint64_t *seed_offset) {
    size_t lines = (length - LOAD_OFFSET) / CACHELINE_SIZE;
    if (lines < 2)
        return false;
    size_t *order = malloc(lines * sizeof(*order));
    if (!order)
        return false;
    for (size_t index = 0; index < lines; ++index)
        order[index] = index;
    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    for (size_t index = lines - 1; index > 0; --index) {
        state = state * UINT64_C(6364136223846793005) + 1;
        size_t selected = (size_t)(state % (index + 1));
        size_t temporary = order[index];
        order[index] = order[selected];
        order[selected] = temporary;
    }
    for (size_t index = 0; index < lines; ++index) {
        size_t current = LOAD_OFFSET + order[index] * CACHELINE_SIZE;
        size_t next = LOAD_OFFSET + order[(index + 1) % lines] * CACHELINE_SIZE;
        *(uint64_t *)(mapping + current) = (uint64_t)next;
    }
    *seed_offset = LOAD_OFFSET + order[0] * CACHELINE_SIZE;
    free(order);
    return true;
}

static bool run_load(const options_t *options, uint8_t *mapping, result_t *result, double cycles_per_ns) {
    if (!pin_cpu(options->cpu_a))
        return false;
    uint64_t current = 0;
    if (!initialize_pointer_cycle(mapping, options->length, &current))
        return false;
    uint64_t *samples = malloc(options->iterations * sizeof(*samples));
    if (!samples)
        return false;
    for (uint64_t index = 0; index < options->iterations; ++index) {
        volatile uint64_t *address = (volatile uint64_t *)(mapping + current);
        if (options->mode == MODE_COLD_LOAD) {
            _mm_clflush((const void *)address);
            _mm_mfence();
            result->flushes++;
        }
        uint64_t start = read_tsc();
        current = *address;
        uint64_t end = read_tsc();
        samples[index] = end - start;
    }
    qsort(samples, options->iterations, sizeof(*samples), compare_u64);
    long double total = 0;
    for (uint64_t index = 0; index < options->iterations; ++index)
        total += samples[index];
    result->operations = options->iterations;
    result->average_ns = (double)(total / options->iterations) / cycles_per_ns;
    result->p50_ns = percentile(samples, options->iterations, 0.50, cycles_per_ns);
    result->p95_ns = percentile(samples, options->iterations, 0.95, cycles_per_ns);
    result->p99_ns = percentile(samples, options->iterations, 0.99, cycles_per_ns);
    result->final_value = current;
    size_t pointer_lines = (options->length - LOAD_OFFSET) / CACHELINE_SIZE;
    result->max_written_offset = LOAD_OFFSET + (pointer_lines - 1) * CACHELINE_SIZE + sizeof(uint64_t);
    free(samples);
    return true;
}

static bool run_two_workers(const options_t *options, shared_state_t *state, result_t *result, double cycles_per_ns) {
    _Atomic int ready = 0;
    _Atomic int start = 0;
    _Atomic int failed = 0;
    uint64_t *tickets_a = NULL;
    uint64_t *tickets_b = NULL;
    void *(*worker)(void *) = NULL;
    if (options->mode == MODE_MESSAGE_PASSING)
        worker = message_worker;
    else if (options->mode == MODE_HANDOFF)
        worker = handoff_worker;
    else if (options->mode == MODE_FETCH_ADD) {
        worker = fetch_add_worker;
        tickets_a = calloc(options->iterations, sizeof(*tickets_a));
        tickets_b = calloc(options->iterations, sizeof(*tickets_b));
        if (!tickets_a || !tickets_b)
            goto fail;
    } else if (options->mode == MODE_CAS) {
        worker = cas_worker;
    }

    worker_args_t args_a = {.state = state,
                            .iterations = options->iterations,
                            .cpu = options->cpu_a,
                            .role = 0,
                            .ready = &ready,
                            .start = &start,
                            .failed = &failed,
                            .tickets = tickets_a};
    worker_args_t args_b = args_a;
    args_b.cpu = options->cpu_b;
    args_b.role = 1;
    args_b.tickets = tickets_b;
    pthread_t thread_a;
    pthread_t thread_b;
    if (pthread_create(&thread_a, NULL, worker, &args_a) != 0)
        goto fail;
    if (pthread_create(&thread_b, NULL, worker, &args_b) != 0) {
        atomic_store(&failed, 1);
        atomic_store(&start, 1);
        pthread_join(thread_a, NULL);
        goto fail;
    }
    uint64_t deadline = monotonic_ns() + WATCHDOG_NS;
    while (atomic_load_explicit(&ready, memory_order_acquire) != 2 && !atomic_load(&failed)) {
        if (monotonic_ns() >= deadline) {
            atomic_store(&failed, 1);
            break;
        }
        _mm_pause();
    }
    uint64_t begin = read_tsc();
    atomic_store_explicit(&start, 1, memory_order_release);
    pthread_join(thread_a, NULL);
    pthread_join(thread_b, NULL);
    uint64_t end = read_tsc();

    result->errors = args_a.errors + args_b.errors + (uint64_t)atomic_load(&failed);
    result->stale = options->mode == MODE_MESSAGE_PASSING ? args_b.errors : 0;
    result->attempts = args_a.attempts + args_b.attempts;
    result->successes = args_a.successes + args_b.successes;
    result->failures = args_a.failures + args_b.failures;
    result->operations = options->mode == MODE_MESSAGE_PASSING ? options->iterations : options->iterations * 2;
    result->average_ns = (double)(end - begin) / cycles_per_ns / (double)result->operations;
    result->max_written_offset = sizeof(*state);

    uint64_t expected_final = options->mode == MODE_MESSAGE_PASSING ? options->iterations : options->iterations * 2;
    if (options->mode == MODE_HANDOFF) {
        result->final_value = atomic_load(&state->payload);
    } else if (options->mode == MODE_FETCH_ADD) {
        result->final_value = atomic_load(&state->counter);
        uint64_t *tickets = realloc(tickets_a, expected_final * sizeof(*tickets));
        if (!tickets)
            goto fail;
        tickets_a = tickets;
        memcpy(tickets_a + options->iterations, tickets_b, options->iterations * sizeof(*tickets_b));
        qsort(tickets_a, expected_final, sizeof(*tickets_a), compare_u64);
        for (uint64_t index = 0; index < expected_final; ++index) {
            if (tickets_a[index] != index)
                result->ticket_errors++;
        }
        result->errors += result->ticket_errors;
    } else if (options->mode == MODE_CAS) {
        result->final_value = atomic_load(&state->cas_value);
        if (result->final_value != expected_final || result->successes != expected_final)
            result->cas_errors++;
        result->errors += result->cas_errors;
    } else {
        result->final_value = atomic_load(&state->payload);
    }
    if (result->final_value != expected_final) {
        result->errors++;
        if (options->mode == MODE_MESSAGE_PASSING)
            result->stale++;
    }
    free(tickets_a);
    free(tickets_b);
    return true;

fail:
    free(tickets_a);
    free(tickets_b);
    return false;
}

static void emit_result(const options_t *options, const result_t *result, double cycles_per_ns) {
    printf("{\"schema\":\"splash.cxlmem-hwcc.v1\",\"mode\":\"%s\",\"backend\":\"%s\","
           "\"cpu_a\":%d,\"cpu_b\":%d,\"iterations\":%" PRIu64 ",\"operations\":%" PRIu64 ","
           "\"errors\":%" PRIu64 ",\"stale\":%" PRIu64 ",\"ticket_errors\":%" PRIu64 ",\"cas_errors\":%" PRIu64
           ",\"final_value\":%" PRIu64 ",\"attempts\":%" PRIu64 ",\"successes\":%" PRIu64 ",\"failures\":%" PRIu64
           ",\"flushes_in_hot_path\":%" PRIu64 ",\"average_ns\":%.3f,\"p50_ns\":%.3f,\"p95_ns\":%.3f,\"p99_ns\":%.3f,"
           "\"tsc_ghz\":%.6f,\"mapped_length\":%zu,\"max_written_offset\":%zu,"
           "\"atomics_lock_free\":true}\n",
           mode_name(options->mode), backend_name(options->backend), options->cpu_a, options->cpu_b,
           options->iterations, result->operations, result->errors, result->stale, result->ticket_errors,
           result->cas_errors, result->final_value, result->attempts, result->successes, result->failures,
           result->flushes, result->average_ns, result->p50_ns, result->p95_ns, result->p99_ns, cycles_per_ns,
           options->length, result->max_written_offset);
}

int main(int argc, char **argv) {
    options_t options;
    if (!parse_options(argc, argv, &options)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    int fd = -1;
    int flags = MAP_SHARED;
    if (options.backend == BACKEND_CXLMEM) {
        fd = open(options.device, O_RDWR | O_CLOEXEC | O_SYNC);
        if (fd < 0) {
            perror("open CXL.mem device");
            return EXIT_FAILURE;
        }
    } else {
        flags |= MAP_ANONYMOUS;
    }
    uint8_t *mapping = mmap(NULL, options.length, PROT_READ | PROT_WRITE, flags, fd, (off_t)options.offset);
    if (fd >= 0)
        close(fd);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }

    memset(mapping, 0, sizeof(shared_state_t));
    shared_state_t *state = (shared_state_t *)mapping;
    atomic_init(&state->payload, 0);
    atomic_init(&state->flag, 0);
    atomic_init(&state->ack, 0);
    atomic_init(&state->turn, 0);
    atomic_init(&state->counter, 0);
    atomic_init(&state->cas_value, 0);
    if (!atomic_is_lock_free(&state->payload) || !atomic_is_lock_free(&state->flag) ||
        !atomic_is_lock_free(&state->counter) || !atomic_is_lock_free(&state->cas_value)) {
        fprintf(stderr, "mapped uint64_t atomics are not lock-free\n");
        munmap(mapping, options.length);
        return EXIT_FAILURE;
    }

    result_t result = {0};
    double cycles_per_ns = calibrate_cycles_per_ns();
    bool success = options.mode == MODE_WARM_LOAD || options.mode == MODE_COLD_LOAD
                       ? run_load(&options, mapping, &result, cycles_per_ns)
                       : run_two_workers(&options, state, &result, cycles_per_ns);
    if (!success) {
        fprintf(stderr, "benchmark setup or execution failed\n");
        munmap(mapping, options.length);
        return EXIT_FAILURE;
    }

    emit_result(&options, &result, cycles_per_ns);
    munmap(mapping, options.length);
    return result.errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
