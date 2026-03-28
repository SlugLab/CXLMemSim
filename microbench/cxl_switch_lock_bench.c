/*
 * cxl_switch_lock_bench.c — CXL 2.0 Switch Coherence & Locking Benchmark
 *
 * Two hosts (A, B) share CXL Type-3 memory through a CXL switch.
 * This benchmark answers three questions for REAL HARDWARE:
 *
 *   Q1. Do PCIe atomics (lock cmpxchg / lock xadd) serialize correctly
 *       at the CXL switch? Can Lamport's bakery algorithm work with
 *       just clflush + sfence, or do we need device-side atomics?
 *
 *   Q2. Without hardware cache coherence across hosts, what's the
 *       correct runtime locking model?  Test three approaches:
 *       (a) Ownership via explicit flush/invalidate (software coherence)
 *       (b) PCIe atomic CAS spinlock (device-serialized)
 *       (c) Lamport bakery (pure loads/stores + flush)
 *
 *   Q3. "Last writer wins" — a race-and-flush protocol where the
 *       winner's data persists and the loser invalidates.  Plus
 *       latency microbenchmarks for every lock variant.
 *
 * Usage:
 *   Host A:  ./cxl_switch_lock_bench A /dev/dax0.0 [iterations]
 *   Host B:  ./cxl_switch_lock_bench B /dev/dax0.0 [iterations]
 *
 * Build:
 *   gcc -O2 -march=native -o cxl_switch_lock_bench cxl_switch_lock_bench.c -lrt -lpthread
 *
 * The test region layout (all cache-line aligned):
 *
 *   [0x0000]  ctrl_block_t         — handshake / coordination
 *   [0x1000]  bakery_t             — Lamport bakery state (Q1)
 *   [0x2000]  cas_lock_t           — CAS spinlock state (Q2b)
 *   [0x3000]  ownership_lock_t     — ownership lock state (Q2a)
 *   [0x4000]  lww_area_t           — last-writer-wins area (Q3)
 *   [0x5000]  shared_counter_t     — mutual-exclusion verification counter
 *   [0x6000]  latency_results_t    — written by each side
 */

#include "dax_litmus_common.h"
#include <sched.h>
#include <x86intrin.h>

/* ========================================================================
 * Cache-line operations for cross-host visibility on CXL Type-3
 *
 * On real CXL hardware behind a switch, each host has its own CPU cache.
 * Writes to CXL memory sit in the local CPU cache and are NOT visible to
 * the other host until explicitly flushed to the device.  Reads may hit
 * stale cache and must be invalidated first.
 *
 * clflush  — flush + invalidate (serializing, slow)
 * clflushopt — flush + invalidate (non-serializing, faster, needs sfence)
 * clwb     — write-back only, line stays cached (needs sfence)
 *
 * For cross-host visibility we need flush (not just write-back) so the
 * other host's subsequent clflush+load sees fresh data from the device.
 * ======================================================================== */

static inline void cxl_flush(volatile void *addr) {
    asm volatile("clflush (%0)" :: "r"(addr) : "memory");
}

static inline void cxl_flushopt(volatile void *addr) {
    asm volatile("clflushopt (%0)" :: "r"(addr) : "memory");
}

static inline void cxl_sfence(void) {
    asm volatile("sfence" ::: "memory");
}

static inline void cxl_mfence(void) {
    asm volatile("mfence" ::: "memory");
}

/* Flush a range and fence */
static inline void cxl_flush_range(void *addr, size_t size) {
    char *p = (char *)((uintptr_t)addr & ~63UL);
    char *end = (char *)addr + size;
    for (; p < end; p += CACHELINE_SIZE)
        cxl_flushopt(p);
    cxl_sfence();
}

/* Invalidate a range (clflush invalidates + flushes) and fence */
static inline void cxl_invalidate_range(void *addr, size_t size) {
    char *p = (char *)((uintptr_t)addr & ~63UL);
    char *end = (char *)addr + size;
    for (; p < end; p += CACHELINE_SIZE)
        cxl_flush(p);
    cxl_mfence();
}

/* Flush a single cache line (for lock word) + sfence */
static inline void cxl_publish(volatile void *addr) {
    cxl_flushopt(addr);
    cxl_sfence();
}

/* Invalidate a single cache line before reading */
static inline void cxl_pull(volatile void *addr) {
    cxl_flush(addr);
    cxl_mfence();
}

/* Spin pause — lighter than nanosleep for tight lock loops */
static inline void spin_pause(void) {
    _mm_pause();
}

/* ========================================================================
 * High-resolution timing via rdtscp
 * ======================================================================== */

static inline uint64_t rdtscp(void) {
    unsigned int aux;
    return __rdtscp(&aux);
}

static double tsc_ghz = 0.0;

static void calibrate_tsc(void) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t c0 = rdtscp();
    /* spin 50ms */
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 50000000 };
    nanosleep(&delay, NULL);
    uint64_t c1 = rdtscp();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    tsc_ghz = (double)(c1 - c0) / ns;
    printf("[TSC] %.3f GHz\n", tsc_ghz);
}

static inline double cycles_to_ns(uint64_t cycles) {
    return (double)cycles / tsc_ghz;
}

/* ========================================================================
 * Shared memory layout structures
 * ======================================================================== */

/* --- Q1: Lamport Bakery Lock ---
 * Classic 2-process bakery: only uses loads, stores, and fences.
 * On a cache-coherent system this works with just memory_order_seq_cst.
 * On CXL without coherence, we must clflush after every store and
 * clflush+mfence before every load to simulate sequential consistency.
 *
 * If the bakery works (no mutex violations) → the flush protocol provides
 * sufficient ordering through the CXL switch.
 * If it fails → plain loads/stores are NOT sequentially consistent even
 * with flushes, and we need device-side atomics (PCIe AtomicOp / CAS).
 */
typedef struct {
    volatile uint32_t choosing[2];  /* choosing[i]: host i is picking a ticket */
    volatile uint32_t ticket[2];    /* ticket[i]: host i's ticket number */
    uint8_t _pad[CACHELINE_SIZE * 2 -
                 2 * sizeof(uint32_t) - 2 * sizeof(uint32_t)];
} __attribute__((aligned(CACHELINE_SIZE))) bakery_t;

/* --- Q2a: Ownership-based lock (software coherence) ---
 * Owner field indicates who holds the lock.  To acquire:
 *   1. Invalidate owner cache line (clflush)
 *   2. Read owner — if FREE, try to write self + flush
 *   3. Re-invalidate and re-read to confirm we won
 * This is effectively a "test-and-set with flush" protocol.
 * No hardware atomics needed — ownership is enforced by flush ordering.
 */
#define OWNER_FREE  0xFFFFFFFFu
#define OWNER_A     0
#define OWNER_B     1

typedef struct {
    volatile uint32_t owner;       /* OWNER_FREE, OWNER_A, or OWNER_B */
    volatile uint32_t generation;  /* incremented on each acquire */
    uint8_t _pad[CACHELINE_SIZE - 2 * sizeof(uint32_t)];
} __attribute__((aligned(CACHELINE_SIZE))) ownership_lock_t;

/* --- Q2b: CAS spinlock (PCIe atomic) ---
 * Uses x86 `lock cmpxchg` on CXL memory.  If the CXL device / switch
 * properly routes PCIe AtomicOp, this CAS is serialized at the device
 * and provides cross-host mutual exclusion without explicit flushes.
 *
 * We test both flavors:
 *   (i)  CAS on cached (WB) mapping — relies on CPU cache coherence
 *        protocol reaching the device (may NOT work cross-host)
 *   (ii) CAS after clflush invalidation — forces the CAS to go to the
 *        device, where the CXL switch serializes it
 */
typedef struct {
    volatile uint32_t lock;        /* 0 = free, 1 = held */
    volatile uint32_t holder;      /* who holds it (OWNER_A/B) */
    uint8_t _pad[CACHELINE_SIZE - 2 * sizeof(uint32_t)];
} __attribute__((aligned(CACHELINE_SIZE))) cas_lock_t;

/* --- Q3: Last-writer-wins area ---
 * Both sides race to write a "claim" value.  After writing, both flush.
 * Then both invalidate and read back.  Whoever's value is visible "won".
 * This tests the raw flush-race semantics of the CXL fabric.
 */
typedef struct {
    volatile uint64_t claim;       /* written by racer */
    volatile uint64_t timestamp;   /* rdtscp at write time */
    volatile uint32_t writer;      /* OWNER_A or OWNER_B */
    uint8_t _pad[CACHELINE_SIZE - sizeof(uint64_t) - sizeof(uint64_t) - sizeof(uint32_t)];
} __attribute__((aligned(CACHELINE_SIZE))) lww_area_t;

/* Shared counter for mutex correctness verification */
typedef struct {
    volatile uint64_t value;
    uint8_t _pad[CACHELINE_SIZE - sizeof(uint64_t)];
} __attribute__((aligned(CACHELINE_SIZE))) shared_counter_t;

/* Per-host latency results */
typedef struct {
    uint64_t samples[8192];
    uint64_t count;
    double min_ns, max_ns, avg_ns, p50_ns, p99_ns;
} __attribute__((aligned(CACHELINE_SIZE))) latency_results_t;

/* --- LogP parameter fitting area ---
 * Dedicated scratch lines for measuring L, o_s, o_r, g independently.
 * Layout: per-cacheline flag/data pairs so each measurement touches
 * exactly one cache line (avoids false sharing).
 */
#define LOGP_MAX_SAMPLES 8192
#define LOGP_PING_LINES  64   /* 64 cache lines for ping-pong */

typedef struct {
    volatile uint64_t flag;  /* 0=idle, odd=A wrote, even=B wrote */
    volatile uint64_t data;
    uint8_t _pad[CACHELINE_SIZE - 2 * sizeof(uint64_t)];
} __attribute__((aligned(CACHELINE_SIZE))) ping_line_t;

typedef struct {
    /* Ping-pong lines for latency measurement */
    ping_line_t ping[LOGP_PING_LINES];

    /* Streaming area for gap/bandwidth measurement */
    volatile uint64_t stream[4096]; /* 32KB streaming buffer */

    /* Contention scaling area: N independent lock-protected counters */
    cas_lock_t  contention_lock[16];
    volatile uint64_t contention_ctr[16];
} __attribute__((aligned(0x1000))) logp_area_t;

/* LogP fitted parameters — output of the calibration */
typedef struct {
    double L_ns;                /* Network latency: half-RTT of flush+load ping-pong */
    double o_s_ns;              /* Sender overhead: flush cost (store → device) */
    double o_r_ns;              /* Receiver overhead: invalidate+load cost (device → CPU) */
    double g_ns;                /* Gap: min inter-message time (1/peak throughput) */
    double bw_gbps;             /* Derived bandwidth: cacheline_size / g */
    double cas_rtt_ns;          /* CAS round-trip (flush+CAS+flush) */
    double congestion_50_ns;    /* Added latency at 50% utilization */
    double congestion_80_ns;    /* Added latency at 80% utilization */
    double congestion_95_ns;    /* Added latency at 95% utilization */
    uint32_t P;                 /* Number of nodes */
} logp_fitted_t;

/* Full shared region */
typedef struct {
    ctrl_block_t       ctrl;               /* 0x0000 */
    uint8_t            _pad0[0x1000 - sizeof(ctrl_block_t)];
    bakery_t           bakery;             /* 0x1000 */
    uint8_t            _pad1[0x1000 - sizeof(bakery_t)];
    cas_lock_t         cas;                /* 0x2000 */
    uint8_t            _pad2[0x1000 - sizeof(cas_lock_t)];
    ownership_lock_t   ownership;          /* 0x3000 */
    uint8_t            _pad3[0x1000 - sizeof(ownership_lock_t)];
    lww_area_t         lww;                /* 0x4000 */
    uint8_t            _pad4[0x1000 - sizeof(lww_area_t)];
    shared_counter_t   counter;            /* 0x5000 */
    uint8_t            _pad5[0x1000 - sizeof(shared_counter_t)];
    latency_results_t  lat[2];             /* 0x6000: [0]=A, [1]=B */
    logp_area_t        logp;               /* after lat[] */
} __attribute__((aligned(0x1000))) shared_region_t;

/* ========================================================================
 * Latency histogram helpers
 * ======================================================================== */

static int cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a, vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static void compute_latency_stats(latency_results_t *r) {
    if (r->count == 0) return;
    uint64_t n = r->count;
    if (n > 8192) n = 8192;
    qsort(r->samples, n, sizeof(uint64_t), cmp_u64);
    r->min_ns = cycles_to_ns(r->samples[0]);
    r->max_ns = cycles_to_ns(r->samples[n - 1]);
    r->p50_ns = cycles_to_ns(r->samples[n / 2]);
    r->p99_ns = cycles_to_ns(r->samples[(uint64_t)(n * 0.99)]);
    double sum = 0;
    for (uint64_t i = 0; i < n; i++) sum += r->samples[i];
    r->avg_ns = cycles_to_ns((uint64_t)(sum / n));
}

static void print_latency(const char *name, latency_results_t *r) {
    printf("  %-30s  n=%-6lu  avg=%8.1f  min=%8.1f  p50=%8.1f  p99=%8.1f  max=%8.1f ns\n",
           name, r->count, r->avg_ns, r->min_ns, r->p50_ns, r->p99_ns, r->max_ns);
}

static inline void record_sample(latency_results_t *r, uint64_t cycles) {
    uint64_t idx = r->count;
    if (idx < 8192) r->samples[idx] = cycles;
    r->count++;
}

/* ========================================================================
 * Q1: Lamport Bakery Lock (with explicit flush/invalidate)
 *
 * This is the litmus test for "can plain stores + clflush provide
 * sequential consistency across a CXL switch?"
 *
 * Protocol (host i acquiring, other host j):
 *   1. choosing[i] = 1;          flush(&choosing[i]);
 *   2. pull(&ticket[j]);
 *      ticket[i] = ticket[j]+1;  flush(&ticket[i]);
 *   3. choosing[i] = 0;          flush(&choosing[i]);
 *   4. pull(&choosing[j]);
 *      while (choosing[j]) { pull(&choosing[j]); }
 *   5. pull(&ticket[j]);
 *      while (ticket[j] != 0 &&
 *             (ticket[j] < ticket[i] ||
 *              (ticket[j] == ticket[i] && j < i)))
 *        { pull(&ticket[j]); }
 *   6. // CRITICAL SECTION
 *   7. ticket[i] = 0;  flush(&ticket[i]);
 * ======================================================================== */

static void bakery_lock(bakery_t *b, int me) {
    int other = 1 - me;

    /* Step 1: I'm choosing */
    b->choosing[me] = 1;
    cxl_publish(&b->choosing[me]);

    /* Step 2: Pick ticket = other's ticket + 1 */
    cxl_pull(&b->ticket[other]);
    uint32_t ot = b->ticket[other];
    b->ticket[me] = ot + 1;
    cxl_publish(&b->ticket[me]);

    /* Step 3: Done choosing */
    b->choosing[me] = 0;
    cxl_publish(&b->choosing[me]);

    /* Step 4: Wait for other to stop choosing */
    cxl_pull(&b->choosing[other]);
    while (b->choosing[other]) {
        spin_pause();
        cxl_pull(&b->choosing[other]);
    }

    /* Step 5: Wait for my turn */
    cxl_pull(&b->ticket[other]);
    while (b->ticket[other] != 0) {
        uint32_t tj = b->ticket[other];
        uint32_t ti = b->ticket[me];  /* local, no flush needed */
        if (tj < ti || (tj == ti && other < me))  {
            /* other has priority — wait */
            spin_pause();
            cxl_pull(&b->ticket[other]);
        } else {
            break;
        }
    }
}

static void bakery_unlock(bakery_t *b, int me) {
    b->ticket[me] = 0;
    cxl_publish(&b->ticket[me]);
}

/* ========================================================================
 * Q2a: Ownership lock (flush-based, no hardware atomics)
 *
 * This models the "all CXL accesses are uncached" scenario.
 * Lock protocol:
 *   1. Invalidate owner line
 *   2. Read owner — if != FREE, spin (invalidate + re-read)
 *   3. Write owner = me; flush
 *   4. Invalidate and re-read to confirm ownership
 *      (another host may have written simultaneously)
 *   5. If owner != me, back off and retry (exponential backoff)
 *   6. CRITICAL SECTION
 *   7. Write owner = FREE; flush
 *
 * This is a "software coherence" protocol — the programmer is responsible
 * for every flush/invalidate.  It works without any hardware coherence
 * but is slower due to the confirm-after-write round-trip.
 * ======================================================================== */

static void ownership_lock(ownership_lock_t *o, int me, latency_results_t *lat) {
    uint32_t my_id = (me == 0) ? OWNER_A : OWNER_B;
    int backoff = 1;

    for (;;) {
        uint64_t t0 = rdtscp();

        /* Wait until FREE */
        for (;;) {
            cxl_pull(&o->owner);
            if (o->owner == OWNER_FREE) break;
            for (int i = 0; i < backoff; i++) spin_pause();
            if (backoff < 1024) backoff <<= 1;
        }

        /* Attempt to claim */
        o->owner = my_id;
        cxl_publish(&o->owner);

        /* Confirm: round-trip to device and back */
        cxl_pull(&o->owner);
        if (o->owner == my_id) {
            /* Won the race */
            o->generation++;
            cxl_publish(&o->generation);
            uint64_t t1 = rdtscp();
            record_sample(lat, t1 - t0);
            return;
        }

        /* Lost — another host overwrote.  Back off and retry. */
        backoff = 1;
    }
}

static void ownership_unlock(ownership_lock_t *o) {
    o->owner = OWNER_FREE;
    cxl_publish(&o->owner);
}

/* ========================================================================
 * Q2b: CAS spinlock (PCIe atomic)
 *
 * Uses x86 `lock cmpxchg` directly on CXL-mapped memory.
 * On real hardware, whether this works depends on:
 *   - Memory type (WB vs UC/WC) of the DAX mapping
 *   - Whether the CXL switch routes PCIe AtomicOp correctly
 *
 * We test two variants:
 *   (i)  "raw CAS" — just __sync_bool_compare_and_swap on WB mapping
 *   (ii) "flush+CAS" — clflush the line, then CAS, then flush again
 *        This forces the operation to the device even if mapped WB.
 * ======================================================================== */

static void cas_lock_raw(cas_lock_t *l, int me, latency_results_t *lat) {
    uint64_t t0 = rdtscp();
    while (!__sync_bool_compare_and_swap(&l->lock, 0, 1)) {
        spin_pause();
    }
    l->holder = (me == 0) ? OWNER_A : OWNER_B;
    uint64_t t1 = rdtscp();
    record_sample(lat, t1 - t0);
}

static void cas_unlock_raw(cas_lock_t *l) {
    __sync_lock_release(&l->lock);
}

static void cas_lock_flush(cas_lock_t *l, int me, latency_results_t *lat) {
    uint64_t t0 = rdtscp();
    for (;;) {
        cxl_pull(&l->lock);              /* invalidate — force next CAS to device */
        if (__sync_bool_compare_and_swap(&l->lock, 0, 1)) {
            cxl_publish(&l->lock);         /* make our CAS visible to other host */
            break;
        }
        spin_pause();
    }
    l->holder = (me == 0) ? OWNER_A : OWNER_B;
    cxl_publish(&l->holder);
    uint64_t t1 = rdtscp();
    record_sample(lat, t1 - t0);
}

static void cas_unlock_flush(cas_lock_t *l) {
    l->lock = 0;
    cxl_publish(&l->lock);
}

/* ========================================================================
 * Q3: Last-Writer-Wins (race-and-flush)
 *
 * Both hosts simultaneously write their claim + timestamp, then flush.
 * After a settle delay, both invalidate and read back.  Whichever
 * host's value is visible "won" — the CXL fabric's last-write ordering
 * determined the outcome.
 *
 * This models the "whoever grabs it keeps it, opponent flushes and
 * re-reads" pattern — useful when contention is rare and the cost of
 * a failed claim (re-read) is acceptable.
 * ======================================================================== */

typedef struct {
    uint32_t wins_a;
    uint32_t wins_b;
    uint32_t ties;
    uint64_t win_lat_sum_a;
    uint64_t win_lat_sum_b;
} lww_stats_t;

/* ========================================================================
 * Bare latency microbenchmarks (no contention, just measure round-trip)
 * ======================================================================== */

static void bench_flush_latency(void *addr, latency_results_t *lat) {
    volatile uint64_t *p = (volatile uint64_t *)addr;
    for (int i = 0; i < 8192; i++) {
        *p = (uint64_t)i;
        uint64_t t0 = rdtscp();
        cxl_flushopt(p);
        cxl_sfence();
        uint64_t t1 = rdtscp();
        record_sample(lat, t1 - t0);
    }
}

static void bench_invalidate_latency(void *addr, latency_results_t *lat) {
    volatile uint64_t *p = (volatile uint64_t *)addr;
    for (int i = 0; i < 8192; i++) {
        uint64_t t0 = rdtscp();
        cxl_flush(p);
        cxl_mfence();
        /* Force a load to complete the round-trip */
        (void)*p;
        uint64_t t1 = rdtscp();
        record_sample(lat, t1 - t0);
    }
}

static void bench_cas_latency(volatile uint32_t *addr, latency_results_t *lat) {
    for (int i = 0; i < 8192; i++) {
        *addr = 0;
        cxl_publish(addr);
        cxl_pull(addr);
        uint64_t t0 = rdtscp();
        __sync_bool_compare_and_swap(addr, 0, 1);
        uint64_t t1 = rdtscp();
        record_sample(lat, t1 - t0);
    }
}

static void bench_flush_cas_latency(volatile uint32_t *addr, latency_results_t *lat) {
    for (int i = 0; i < 8192; i++) {
        *addr = 0;
        cxl_publish(addr);
        cxl_pull(addr);
        uint64_t t0 = rdtscp();
        cxl_pull(addr);
        __sync_bool_compare_and_swap(addr, 0, 1);
        cxl_publish(addr);
        uint64_t t1 = rdtscp();
        record_sample(lat, t1 - t0);
    }
}

static void bench_store_load_roundtrip(void *addr, latency_results_t *lat) {
    volatile uint64_t *p = (volatile uint64_t *)addr;
    /* Measure: store → flush → invalidate → load (full cross-host round-trip) */
    for (int i = 0; i < 8192; i++) {
        uint64_t t0 = rdtscp();
        *p = (uint64_t)i;
        cxl_publish(p);        /* flush to device */
        cxl_pull(p);           /* invalidate + reload from device */
        (void)*p;
        uint64_t t1 = rdtscp();
        record_sample(lat, t1 - t0);
    }
}

/* Forward declarations */
static void barrier(ctrl_block_t *ctrl, role_t role);
static void handshake(ctrl_block_t *ctrl, role_t role);

/* ========================================================================
 * LogP Parameter Fitting — measure L, o_s, o_r, g, and contention curve
 *
 * These tests are designed to produce values that plug directly into
 * CXLMemSim's LogPConfig{L, o_s, o_r, g, P} and FabricLinkConfig.
 *
 * Methodology:
 *   o_s = clflushopt+sfence latency (cost of pushing one store to device)
 *   o_r = clflush+mfence+load latency (cost of pulling one load from device)
 *   L   = (ping-pong RTT - o_s - o_r) / 2
 *         where RTT is measured with cross-host flag exchange
 *   g   = min inter-message time under sustained streaming
 *   BW  = CACHELINE_SIZE / g
 *
 * Contention curve: measure lock acquire latency at different offered
 * loads (1, 2, 4, 8, 16 concurrent cachelines) to fit the non-linear
 * congestion model: FabricLink::get_congestion_delay().
 * ======================================================================== */

/*
 * Measure o_s: sender overhead = time to flush a dirty cacheline to device.
 * This is the cost the sender pays to make a write visible.
 */
static double measure_o_s(void *scratch, int warmup) {
    volatile uint64_t *p = (volatile uint64_t *)scratch;
    latency_results_t lat = { .count = 0 };

    /* Warmup to prime TLB and page tables */
    for (int i = 0; i < warmup; i++) {
        *p = (uint64_t)i;
        cxl_flushopt(p);
        cxl_sfence();
    }

    for (int i = 0; i < LOGP_MAX_SAMPLES; i++) {
        *p = (uint64_t)i;          /* dirty the line */
        uint64_t t0 = rdtscp();
        cxl_flushopt(p);
        cxl_sfence();
        uint64_t t1 = rdtscp();
        record_sample(&lat, t1 - t0);
    }
    compute_latency_stats(&lat);
    return lat.p50_ns;  /* Use median: robust against outliers */
}

/*
 * Measure o_r: receiver overhead = time to invalidate + reload from device.
 * This is the cost the receiver pays to read a fresh value.
 */
static double measure_o_r(void *scratch, int warmup) {
    volatile uint64_t *p = (volatile uint64_t *)scratch;
    latency_results_t lat = { .count = 0 };

    *p = 42;
    cxl_publish(p);

    for (int i = 0; i < warmup; i++) {
        cxl_flush(p);
        cxl_mfence();
        (void)*p;
    }

    for (int i = 0; i < LOGP_MAX_SAMPLES; i++) {
        uint64_t t0 = rdtscp();
        cxl_flush(p);
        cxl_mfence();
        (void)*p;  /* Force load from device */
        uint64_t t1 = rdtscp();
        record_sample(&lat, t1 - t0);
    }
    compute_latency_stats(&lat);
    return lat.p50_ns;
}

/*
 * Measure L: network latency via cross-host ping-pong.
 *
 * Protocol (alternating flag on a single cacheline):
 *   A: write flag=seq (odd), flush.  Wait for flag=seq+1 (B's ack).
 *   B: wait for flag=seq (odd, A wrote).  Write flag=seq+1 (even), flush.
 *
 * One round = A_flush + A_propagation + B_invalidate + B_load
 *           + B_flush + B_propagation + A_invalidate + A_load
 *           = 2*(o_s + L + o_r)
 *
 * So L = (RTT/2) - (o_s + o_r)/2, but more precisely:
 *   L = (RTT - 2*o_s - 2*o_r) / 2
 */
static double measure_L_pingpong(ping_line_t *pl, role_t role,
                                  int rounds, double o_s_val, double o_r_val) {
    latency_results_t lat = { .count = 0 };

    if (role == ROLE_A) {
        for (int i = 0; i < rounds; i++) {
            uint64_t seq = (uint64_t)(i * 2 + 1);  /* odd = A's turn */

            uint64_t t0 = rdtscp();
            pl->flag = seq;
            cxl_publish(&pl->flag);

            /* Wait for B's reply (even) */
            for (;;) {
                cxl_pull(&pl->flag);
                if (pl->flag == seq + 1) break;
                spin_pause();
            }
            uint64_t t1 = rdtscp();

            record_sample(&lat, t1 - t0);
        }
    } else {
        for (int i = 0; i < rounds; i++) {
            uint64_t seq = (uint64_t)(i * 2 + 1);

            /* Wait for A's ping (odd) */
            for (;;) {
                cxl_pull(&pl->flag);
                if (pl->flag == seq) break;
                spin_pause();
            }

            /* Reply (even) */
            pl->flag = seq + 1;
            cxl_publish(&pl->flag);
        }
    }

    if (role == ROLE_A) {
        compute_latency_stats(&lat);
        double rtt = lat.p50_ns;
        double L = (rtt - 2.0 * o_s_val - 2.0 * o_r_val) / 2.0;
        if (L < 0) L = 0;
        return L;
    }
    return 0.0;
}

/*
 * Measure g: gap = minimum inter-message time.
 *
 * Single-sender streaming: A writes N cache lines back-to-back with
 * flush after each.  g = total_time / N.  This measures the sustained
 * throughput bottleneck at the CXL link.
 */
static double measure_g_streaming(volatile uint64_t *stream, int lines) {
    /* Warmup */
    for (int i = 0; i < lines; i++) {
        stream[i * (CACHELINE_SIZE / sizeof(uint64_t))] = 0;
        cxl_flushopt(&stream[i * (CACHELINE_SIZE / sizeof(uint64_t))]);
    }
    cxl_sfence();

    uint64_t t0 = rdtscp();
    for (int i = 0; i < lines; i++) {
        stream[i * (CACHELINE_SIZE / sizeof(uint64_t))] = (uint64_t)i;
        cxl_flushopt(&stream[i * (CACHELINE_SIZE / sizeof(uint64_t))]);
    }
    cxl_sfence();
    uint64_t t1 = rdtscp();

    double total_ns = cycles_to_ns(t1 - t0);
    return total_ns / lines;
}

/*
 * Measure contention curve: lock acquire latency as a function of
 * offered load.  Both hosts hammer N independent lock+counter pairs.
 *
 * At stride=1, both hosts hit the SAME lock → maximum contention.
 * At stride=16, each host has its own lock → zero contention.
 * Intermediate strides measure the congestion gradient.
 *
 * This maps to CXLMemSim's FabricLink congestion model:
 *   utilization < 0.5  → 0ns
 *   0.5-0.8            → linear 0-20ns
 *   0.8-1.0            → steep 20-100ns
 */
static void measure_contention_curve(logp_area_t *lp, role_t role,
                                      uint32_t iters_per_point,
                                      double results_ns[4]) {
    /* Test points: 1, 2, 4, 8 contended locks out of 16 */
    int test_nlocks[] = { 1, 2, 4, 8 };
    int me = (int)role;

    for (int t = 0; t < 4; t++) {
        int nlocks = test_nlocks[t];
        latency_results_t lat = { .count = 0 };

        for (uint32_t i = 0; i < iters_per_point; i++) {
            int idx = (int)(i % (unsigned)nlocks);  /* which lock to hit */

            uint64_t t0 = rdtscp();

            /* Acquire via flush+CAS */
            for (;;) {
                cxl_pull(&lp->contention_lock[idx].lock);
                if (__sync_bool_compare_and_swap(&lp->contention_lock[idx].lock, 0, 1)) {
                    cxl_publish(&lp->contention_lock[idx].lock);
                    break;
                }
                spin_pause();
            }

            uint64_t t1 = rdtscp();

            /* Critical section: increment counter */
            cxl_pull(&lp->contention_ctr[idx]);
            lp->contention_ctr[idx]++;
            cxl_publish(&lp->contention_ctr[idx]);

            /* Release */
            lp->contention_lock[idx].lock = 0;
            cxl_publish(&lp->contention_lock[idx].lock);

            record_sample(&lat, t1 - t0);
        }

        compute_latency_stats(&lat);
        results_ns[t] = lat.p50_ns;
        printf("  [%c] nlocks=%-2d  p50=%8.1f  p99=%8.1f  avg=%8.1f ns\n",
               me == 0 ? 'A' : 'B', nlocks, lat.p50_ns, lat.p99_ns, lat.avg_ns);
    }
}

/*
 * Measure cross-host CAS RTT: the time for a contested CAS that
 * must go through the CXL switch.  A holds the lock, B tries CAS,
 * fails, A releases, B succeeds.  Measures the full contested path.
 */
static double measure_cas_contested_rtt(cas_lock_t *l, role_t role, int rounds) {
    latency_results_t lat = { .count = 0 };

    if (role == ROLE_A) {
        for (int i = 0; i < rounds; i++) {
            /* A acquires */
            cxl_pull(&l->lock);
            while (!__sync_bool_compare_and_swap(&l->lock, 0, 1)) {
                cxl_pull(&l->lock);
                spin_pause();
            }
            cxl_publish(&l->lock);

            /* Hold briefly so B sees contention */
            for (int j = 0; j < 10; j++) spin_pause();

            /* Release */
            l->lock = 0;
            cxl_publish(&l->lock);

            /* Wait for B to take and release */
            for (;;) {
                cxl_pull(&l->lock);
                if (l->lock == 0) {
                    /* Check if B's generation flag changed */
                    cxl_pull(&l->holder);
                    if (l->holder == OWNER_B) break;
                }
                spin_pause();
            }
            l->holder = OWNER_FREE;
            cxl_publish(&l->holder);
        }
    } else {
        for (int i = 0; i < rounds; i++) {
            /* B tries to acquire (contested) */
            uint64_t t0 = rdtscp();
            cxl_pull(&l->lock);
            while (!__sync_bool_compare_and_swap(&l->lock, 0, 1)) {
                cxl_pull(&l->lock);
                spin_pause();
            }
            cxl_publish(&l->lock);
            uint64_t t1 = rdtscp();

            record_sample(&lat, t1 - t0);

            l->holder = OWNER_B;
            cxl_publish(&l->holder);

            /* Release */
            l->lock = 0;
            cxl_publish(&l->lock);

            /* Wait for A to ack */
            for (;;) {
                cxl_pull(&l->holder);
                if (l->holder == OWNER_FREE) break;
                spin_pause();
            }
        }
    }

    if (role == ROLE_B) {
        compute_latency_stats(&lat);
        return lat.p50_ns;
    }
    return 0.0;
}

/*
 * Full LogP calibration suite.
 * Outputs parameters for CXLMemSim's LogPConfig and FabricLinkConfig.
 */
static void run_logp_calibration(shared_region_t *r, role_t role, uint32_t iters) {
    logp_area_t *lp = &r->logp;
    logp_fitted_t fit = { 0 };
    fit.P = 2;

    printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  LogP Parameter Fitting for CXLMemSim Switch Emulation     ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    /* --- Phase 1: Single-host measurements (o_s, o_r, g) --- */
    printf("\n--- Phase 1: Single-host overhead (no cross-host traffic) ---\n");

    fit.o_s_ns = measure_o_s((void *)&lp->ping[0], 1000);
    printf("  [%c] o_s (sender overhead, clflushopt+sfence) = %.1f ns\n",
           role == ROLE_A ? 'A' : 'B', fit.o_s_ns);

    fit.o_r_ns = measure_o_r((void *)&lp->ping[1], 1000);
    printf("  [%c] o_r (receiver overhead, clflush+mfence+load) = %.1f ns\n",
           role == ROLE_A ? 'A' : 'B', fit.o_r_ns);

    /* g from streaming */
    fit.g_ns = measure_g_streaming(lp->stream, 512);
    fit.bw_gbps = (double)CACHELINE_SIZE / fit.g_ns;  /* GB/s */
    printf("  [%c] g (gap, streaming flush) = %.1f ns → BW = %.2f GB/s\n",
           role == ROLE_A ? 'A' : 'B', fit.g_ns, fit.bw_gbps);

    barrier(&r->ctrl, role);

    /* --- Phase 2: Cross-host ping-pong (L) --- */
    printf("\n--- Phase 2: Cross-host ping-pong (network latency L) ---\n");

    /* Init ping line */
    if (role == ROLE_A) {
        lp->ping[2].flag = 0;
        cxl_publish(&lp->ping[2].flag);
    }
    barrier(&r->ctrl, role);

    int ping_rounds = (iters > LOGP_MAX_SAMPLES) ? LOGP_MAX_SAMPLES : (int)iters;
    fit.L_ns = measure_L_pingpong(&lp->ping[2], role, ping_rounds,
                                   fit.o_s_ns, fit.o_r_ns);
    if (role == ROLE_A) {
        double rtt = 2.0 * (fit.o_s_ns + fit.L_ns + fit.o_r_ns);
        printf("  [A] RTT = %.1f ns, L (propagation) = %.1f ns\n", rtt, fit.L_ns);
        printf("  [A] p2p_latency = o_s + L + o_r = %.1f + %.1f + %.1f = %.1f ns\n",
               fit.o_s_ns, fit.L_ns, fit.o_r_ns,
               fit.o_s_ns + fit.L_ns + fit.o_r_ns);
    }
    barrier(&r->ctrl, role);

    /* --- Phase 3: Cross-host CAS RTT (contested atomic) --- */
    printf("\n--- Phase 3: Cross-host contested CAS round-trip ---\n");

    if (role == ROLE_A) {
        lp->contention_lock[0].lock = 0;
        lp->contention_lock[0].holder = OWNER_FREE;
        cxl_flush_range((void *)&lp->contention_lock[0], sizeof(cas_lock_t));
    }
    barrier(&r->ctrl, role);

    int cas_rounds = (iters > 2000) ? 2000 : (int)iters;
    fit.cas_rtt_ns = measure_cas_contested_rtt(&lp->contention_lock[0],
                                                role, cas_rounds);
    /* B measured, broadcast result via shared memory */
    if (role == ROLE_B) {
        uint64_t tmp;
        memcpy(&tmp, &fit.cas_rtt_ns, sizeof(tmp));
        lp->ping[3].data = tmp;
        cxl_publish(&lp->ping[3].data);
    }
    barrier(&r->ctrl, role);
    if (role == ROLE_A) {
        cxl_pull(&lp->ping[3].data);
        uint64_t tmp = lp->ping[3].data;
        memcpy(&fit.cas_rtt_ns, &tmp, sizeof(fit.cas_rtt_ns));
    }
    printf("  [%c] Contested CAS RTT = %.1f ns\n",
           role == ROLE_A ? 'A' : 'B', fit.cas_rtt_ns);

    barrier(&r->ctrl, role);

    /* --- Phase 4: Contention curve (congestion model fitting) --- */
    printf("\n--- Phase 4: Contention curve (varying lock fanout) ---\n");
    printf("  nlocks=1 → max contention, nlocks=8 → low contention\n");

    /* Init all contention locks */
    if (role == ROLE_A) {
        for (int i = 0; i < 16; i++) {
            lp->contention_lock[i].lock = 0;
            lp->contention_lock[i].holder = OWNER_FREE;
            lp->contention_ctr[i] = 0;
        }
        cxl_flush_range((void *)lp->contention_lock,
                         sizeof(cas_lock_t) * 16);
        cxl_flush_range((void *)lp->contention_ctr,
                         sizeof(uint64_t) * 16);
    }
    barrier(&r->ctrl, role);

    double contention_ns[4];
    uint32_t cont_iters = (iters > 2000) ? 2000 : iters;
    measure_contention_curve(lp, role, cont_iters, contention_ns);

    barrier(&r->ctrl, role);

    /* Map contention results to utilization levels.
     * nlocks=1 → ~100% utilization (both hosts hit same lock)
     * nlocks=2 → ~50% collision probability
     * nlocks=4 → ~25% collision probability
     * nlocks=8 → ~12.5% collision probability
     *
     * The added congestion = contention_ns[i] - uncontended_cas
     * This fits CXLMemSim's congestion_delay(utilization) curve.
     */
    double uncontended = contention_ns[3]; /* nlocks=8 ≈ uncontended */
    fit.congestion_50_ns  = (contention_ns[2] > uncontended)
                            ? contention_ns[2] - uncontended : 0.0;
    fit.congestion_80_ns  = (contention_ns[1] > uncontended)
                            ? contention_ns[1] - uncontended : 0.0;
    fit.congestion_95_ns  = (contention_ns[0] > uncontended)
                            ? contention_ns[0] - uncontended : 0.0;

    /* --- Summary: output CXLMemSim configuration --- */
    printf("\n┌──────────────────────────────────────────────────────────────┐\n");
    printf("│  FITTED LogP PARAMETERS — paste into CXLMemSim config      │\n");
    printf("├──────────────────────────────────────────────────────────────┤\n");
    printf("│  LogPConfig {                                               │\n");
    printf("│    .L   = %7.1f,  // Network latency (ns)                 │\n", fit.L_ns);
    printf("│    .o_s = %7.1f,  // Sender overhead (ns)                 │\n", fit.o_s_ns);
    printf("│    .o_r = %7.1f,  // Receiver overhead (ns)               │\n", fit.o_r_ns);
    printf("│    .g   = %7.1f,  // Gap (ns) → %.2f GB/s              │\n", fit.g_ns, fit.bw_gbps);
    printf("│    .P   = %7u,  // Processors                           │\n", fit.P);
    printf("│  };                                                         │\n");
    printf("│                                                              │\n");
    printf("│  FabricLinkConfig {                                         │\n");
    printf("│    .latency_ns     = %7.1f, // L (per-hop)               │\n", fit.L_ns);
    printf("│    .bandwidth_gbps = %7.2f, // 64B / g                   │\n", fit.bw_gbps);
    printf("│    .max_credits    = 32,                                    │\n");
    printf("│  };                                                         │\n");
    printf("│                                                              │\n");
    printf("│  Congestion model fit:                                      │\n");
    printf("│    ~25%% util → +%.1f ns  (nlocks=4)                       │\n", fit.congestion_50_ns);
    printf("│    ~50%% util → +%.1f ns  (nlocks=2)                       │\n", fit.congestion_80_ns);
    printf("│    ~100%% util → +%.1f ns (nlocks=1)                       │\n", fit.congestion_95_ns);
    printf("│                                                              │\n");
    printf("│  Contested CAS RTT = %.1f ns                               │\n", fit.cas_rtt_ns);
    printf("│  p2p_latency = %.1f ns (o_s + L + o_r)                    │\n",
           fit.o_s_ns + fit.L_ns + fit.o_r_ns);
    printf("└──────────────────────────────────────────────────────────────┘\n");
}

/* ========================================================================
 * Handshake — reuse the existing litmus test pattern
 * ======================================================================== */

static void handshake(ctrl_block_t *ctrl, role_t role) {
    if (role == ROLE_A) {
        /* Clear everything */
        memset((void *)ctrl, 0, sizeof(*ctrl));
        cxl_flush_range((void *)ctrl, sizeof(*ctrl));
        atomic_store_explicit(&ctrl->ready_a, 1, memory_order_relaxed);
        cxl_publish(&ctrl->ready_a);

        printf("[A] Waiting for host B...\n");
        for (;;) {
            cxl_pull(&ctrl->ready_b);
            if (atomic_load_explicit(&ctrl->ready_b, memory_order_relaxed) != 0) break;
            busy_pause();
        }
        atomic_store_explicit(&ctrl->magic, 0xC1BEu, memory_order_relaxed);
        cxl_publish(&ctrl->magic);
        printf("[A] Host B connected. Starting benchmark.\n");
    } else {
        printf("[B] Waiting for host A...\n");
        atomic_store_explicit(&ctrl->ready_b, 1, memory_order_relaxed);
        cxl_publish(&ctrl->ready_b);

        for (;;) {
            cxl_pull(&ctrl->ready_a);
            if (atomic_load_explicit(&ctrl->ready_a, memory_order_relaxed) != 0) break;
            busy_pause();
        }
        for (;;) {
            cxl_pull(&ctrl->magic);
            if (atomic_load_explicit(&ctrl->magic, memory_order_relaxed) == 0xC1BEu) break;
            busy_pause();
        }
        printf("[B] Connected. Starting benchmark.\n");
    }
}

/* Barrier: both sides signal done, then wait for other */
static void barrier(ctrl_block_t *ctrl, role_t role) {
    if (role == ROLE_A) {
        atomic_fetch_add_explicit(&ctrl->seq, 1, memory_order_relaxed);
        cxl_publish(&ctrl->seq);
        uint32_t target = atomic_load_explicit(&ctrl->seq, memory_order_relaxed);
        /* Wait for B to also increment (target becomes even) */
        for (;;) {
            cxl_pull(&ctrl->seq);
            uint32_t v = atomic_load_explicit(&ctrl->seq, memory_order_relaxed);
            if (v >= target + 1) break;
            busy_pause();
        }
    } else {
        /* Wait for A to increment first, then increment ourselves */
        uint32_t prev;
        for (;;) {
            cxl_pull(&ctrl->seq);
            prev = atomic_load_explicit(&ctrl->seq, memory_order_relaxed);
            if (prev & 1) break;  /* A incremented to odd */
            busy_pause();
        }
        atomic_fetch_add_explicit(&ctrl->seq, 1, memory_order_relaxed);
        cxl_publish(&ctrl->seq);
    }
}

/* ========================================================================
 * Test runners
 * ======================================================================== */

static void run_q1_bakery(shared_region_t *r, role_t role, uint32_t iters) {
    int me = (int)role;
    bakery_t *b = &r->bakery;
    shared_counter_t *cnt = &r->counter;

    printf("\n=== Q1: Lamport Bakery (flush-based sequential consistency) ===\n");

    /* Init */
    if (role == ROLE_A) {
        b->choosing[0] = b->choosing[1] = 0;
        b->ticket[0] = b->ticket[1] = 0;
        cnt->value = 0;
        cxl_flush_range((void *)b, sizeof(*b));
        cxl_flush_range((void *)cnt, sizeof(*cnt));
    }
    barrier(&r->ctrl, role);

    uint32_t violations = 0;
    latency_results_t lat = { .count = 0 };

    for (uint32_t i = 0; i < iters; i++) {
        uint64_t t0 = rdtscp();
        bakery_lock(b, me);
        uint64_t t1 = rdtscp();

        /* Critical section: increment shared counter */
        cxl_pull(&cnt->value);
        uint64_t v = cnt->value;
        cnt->value = v + 1;
        cxl_publish(&cnt->value);

        /* Check for concurrent access (should never happen under mutex) */
        cxl_pull(&cnt->value);
        if (cnt->value != v + 1) violations++;

        bakery_unlock(b, me);

        record_sample(&lat, t1 - t0);
    }

    barrier(&r->ctrl, role);

    /* Report */
    if (role == ROLE_A) {
        cxl_pull(&cnt->value);
        uint64_t final_val = cnt->value;
        uint64_t expected = (uint64_t)iters * 2;
        printf("  Counter: final=%lu, expected=%lu → %s\n",
               final_val, expected,
               final_val == expected ? "PASS (bakery works!)" : "FAIL (ordering violation!)");
        if (final_val != expected)
            printf("  *** Lamport bakery FAILED: flush+sfence does NOT provide SC across CXL switch\n");
    }
    printf("  [%c] violations=%u\n", role == ROLE_A ? 'A' : 'B', violations);
    compute_latency_stats(&lat);
    print_latency("bakery_lock acquire", &lat);
}

static void run_q2b_cas(shared_region_t *r, role_t role, uint32_t iters, int use_flush) {
    int me = (int)role;
    cas_lock_t *l = &r->cas;
    shared_counter_t *cnt = &r->counter;

    printf("\n=== Q2b: CAS Spinlock (%s) ===\n", use_flush ? "flush+CAS" : "raw CAS");

    if (role == ROLE_A) {
        l->lock = 0; l->holder = OWNER_FREE;
        cnt->value = 0;
        cxl_flush_range((void *)l, sizeof(*l));
        cxl_flush_range((void *)cnt, sizeof(*cnt));
    }
    barrier(&r->ctrl, role);

    latency_results_t lat = { .count = 0 };

    for (uint32_t i = 0; i < iters; i++) {
        if (use_flush)
            cas_lock_flush(l, me, &lat);
        else
            cas_lock_raw(l, me, &lat);

        /* Critical section */
        cxl_pull(&cnt->value);
        uint64_t v = cnt->value;
        cnt->value = v + 1;
        cxl_publish(&cnt->value);

        if (use_flush)
            cas_unlock_flush(l);
        else
            cas_unlock_raw(l);
    }

    barrier(&r->ctrl, role);

    if (role == ROLE_A) {
        cxl_pull(&cnt->value);
        uint64_t final_val = cnt->value;
        uint64_t expected = (uint64_t)iters * 2;
        printf("  Counter: final=%lu, expected=%lu → %s\n",
               final_val, expected,
               final_val == expected ? "PASS" : "FAIL (atomics broken!)");
        if (!use_flush && final_val != expected)
            printf("  *** Raw CAS FAILED: lock cmpxchg is NOT serialized across CXL switch\n"
                   "  *** → Must use flush+CAS variant for cross-host locking\n");
    }
    compute_latency_stats(&lat);
    print_latency(use_flush ? "cas_flush acquire" : "cas_raw acquire", &lat);
}

static void run_q2a_ownership(shared_region_t *r, role_t role, uint32_t iters) {
    int me = (int)role;
    ownership_lock_t *o = &r->ownership;
    shared_counter_t *cnt = &r->counter;

    printf("\n=== Q2a: Ownership Lock (software coherence, no atomics) ===\n");

    if (role == ROLE_A) {
        o->owner = OWNER_FREE;
        o->generation = 0;
        cnt->value = 0;
        cxl_flush_range((void *)o, sizeof(*o));
        cxl_flush_range((void *)cnt, sizeof(*cnt));
    }
    barrier(&r->ctrl, role);

    latency_results_t lat = { .count = 0 };

    for (uint32_t i = 0; i < iters; i++) {
        ownership_lock(o, me, &lat);

        /* Critical section */
        cxl_pull(&cnt->value);
        uint64_t v = cnt->value;
        cnt->value = v + 1;
        cxl_publish(&cnt->value);

        ownership_unlock(o);
    }

    barrier(&r->ctrl, role);

    if (role == ROLE_A) {
        cxl_pull(&cnt->value);
        uint64_t final_val = cnt->value;
        uint64_t expected = (uint64_t)iters * 2;
        printf("  Counter: final=%lu, expected=%lu → %s\n",
               final_val, expected,
               final_val == expected ? "PASS" : "FAIL");
        if (final_val != expected)
            printf("  *** Ownership lock FAILED: flush-confirm protocol is insufficient\n"
                   "  *** → CXL switch does not serialize non-atomic writes\n");
    }
    compute_latency_stats(&lat);
    print_latency("ownership acquire", &lat);
}

static void run_q3_lww(shared_region_t *r, role_t role, uint32_t iters) {
    lww_area_t *a = &r->lww;
    uint32_t my_id = (role == ROLE_A) ? OWNER_A : OWNER_B;

    printf("\n=== Q3: Last-Writer-Wins (race-and-flush) ===\n");

    lww_stats_t stats = { 0 };

    if (role == ROLE_A) {
        a->claim = 0; a->writer = OWNER_FREE; a->timestamp = 0;
        cxl_flush_range((void *)a, sizeof(*a));
    }
    barrier(&r->ctrl, role);

    for (uint32_t i = 0; i < iters; i++) {
        /* Synchronize start of each round */
        barrier(&r->ctrl, role);

        /* RACE: both write simultaneously */
        uint64_t ts = rdtscp();
        a->claim = (uint64_t)my_id + ((uint64_t)i << 8);
        a->writer = my_id;
        a->timestamp = ts;
        /* Flush our write to the device */
        cxl_publish(&a->writer);
        cxl_flush_range((void *)a, sizeof(*a));

        /* Small settle delay — let the CXL fabric resolve the race */
        for (int j = 0; j < 100; j++) spin_pause();

        /* Read back: who won? */
        cxl_invalidate_range((void *)a, sizeof(*a));
        uint32_t winner = a->writer;

        if (winner == OWNER_A)
            stats.wins_a++;
        else if (winner == OWNER_B)
            stats.wins_b++;
        else
            stats.ties++;
    }

    barrier(&r->ctrl, role);

    printf("  [%c] wins_A=%u  wins_B=%u  unclear=%u  (of %u rounds)\n",
           role == ROLE_A ? 'A' : 'B', stats.wins_a, stats.wins_b, stats.ties, iters);
    if (role == ROLE_A) {
        if (stats.wins_a + stats.wins_b == iters)
            printf("  → LWW is deterministic: every round has a clear winner\n");
        else
            printf("  → LWW has ambiguous rounds: flush ordering is not total\n");
    }
}

static void run_latency_microbench(shared_region_t *r, role_t role) {
    printf("\n=== Latency Microbenchmarks (single-host, no contention) ===\n");

    /* Use the lww area as scratch space */
    void *scratch = (void *)&r->lww;
    latency_results_t lat;

    /* 1. clflushopt + sfence latency */
    memset(&lat, 0, sizeof(lat));
    bench_flush_latency(scratch, &lat);
    compute_latency_stats(&lat);
    print_latency("clflushopt+sfence", &lat);

    /* 2. clflush + mfence + load (invalidate) latency */
    memset(&lat, 0, sizeof(lat));
    bench_invalidate_latency(scratch, &lat);
    compute_latency_stats(&lat);
    print_latency("clflush+mfence+load", &lat);

    /* 3. store → flush → invalidate → load (full round-trip) */
    memset(&lat, 0, sizeof(lat));
    bench_store_load_roundtrip(scratch, &lat);
    compute_latency_stats(&lat);
    print_latency("store-flush-inv-load RT", &lat);

    /* 4. Raw CAS latency (uncontended) */
    memset(&lat, 0, sizeof(lat));
    bench_cas_latency(&r->cas.lock, &lat);
    compute_latency_stats(&lat);
    print_latency("CAS uncontended (raw)", &lat);

    /* 5. Flush+CAS latency (uncontended) */
    memset(&lat, 0, sizeof(lat));
    bench_flush_cas_latency(&r->cas.lock, &lat);
    compute_latency_stats(&lat);
    print_latency("CAS uncontended (flush)", &lat);

    (void)role;
}

/* ========================================================================
 * Main
 * ======================================================================== */

static void usage(const char *argv0) {
    fprintf(stderr,
        "CXL Switch Coherence & Locking Benchmark (real hardware)\n\n"
        "Usage: %s <role:A|B> <path:/dev/daxX.Y> [iterations] [offset]\n\n"
        "  role       A = initiator host, B = responder host\n"
        "  path       DAX device shared via CXL switch (e.g. /dev/dax0.0)\n"
        "  iterations Per-test iteration count (default: 10000)\n"
        "  offset     Byte offset into DAX device (default: 0)\n\n"
        "Run on BOTH hosts simultaneously.  A initializes, B waits.\n",
        argv0);
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    role_t role = parse_role(argv[1]);
    const char *path = argv[2];
    uint32_t iters = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 10000u;
    size_t offset = (argc > 4) ? strtoull(argv[4], NULL, 0) : 0ULL;

    printf("CXL Switch Lock Benchmark — role=%c, device=%s, iters=%u\n",
           role == ROLE_A ? 'A' : 'B', path, iters);

    /* Map shared CXL memory */
    size_t size = 0;  /* auto-detect from sysfs */
    map_handle_t mh;
    void *region = map_region(path, &size, offset, &mh);
    if (!region || size < sizeof(shared_region_t)) {
        fprintf(stderr, "Failed to map region (need %zu bytes, got %zu)\n",
                sizeof(shared_region_t), size);
        return 2;
    }
    shared_region_t *r = (shared_region_t *)region;
    printf("Mapped %zu bytes at %p\n", size, region);

    calibrate_tsc();

    /* Handshake between hosts */
    handshake(&r->ctrl, role);

    /* ---- Run all tests ---- */

    /* Single-host latency first (no coordination needed) */
    run_latency_microbench(r, role);
    barrier(&r->ctrl, role);

    /* Q1: Lamport bakery — tests flush-based SC */
    run_q1_bakery(r, role, iters);
    barrier(&r->ctrl, role);

    /* Q2b: CAS spinlock — raw (tests if lock cmpxchg reaches device) */
    run_q2b_cas(r, role, iters, /*use_flush=*/0);
    barrier(&r->ctrl, role);

    /* Q2b: CAS spinlock — with flush (guaranteed device-level serialization) */
    run_q2b_cas(r, role, iters, /*use_flush=*/1);
    barrier(&r->ctrl, role);

    /* Q2a: Ownership lock — pure software coherence */
    run_q2a_ownership(r, role, iters);
    barrier(&r->ctrl, role);

    /* Q3: Last-writer-wins */
    run_q3_lww(r, role, iters > 1000 ? 1000 : iters);
    barrier(&r->ctrl, role);

    /* LogP parameter fitting for CXLMemSim */
    run_logp_calibration(r, role, iters);

    /* Summary */
    printf("\n============================================================\n");
    printf("All tests complete.\n");
    printf("============================================================\n");

    unmap_region(&mh);
    return 0;
}
