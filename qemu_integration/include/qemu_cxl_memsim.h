#ifndef QEMU_CXL_MEMSIM_H
#define QEMU_CXL_MEMSIM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CACHELINE_SIZE 64

/* Operation types - matching server's OP_* constants */
#define CXL_READ_OP         0
#define CXL_WRITE_OP        1
#define CXL_GET_SHM_INFO_OP 2
#define CXL_ATOMIC_FAA_OP   3  /* Fetch-and-Add */
#define CXL_ATOMIC_CAS_OP   4  /* Compare-and-Swap */
#define CXL_FENCE_OP        5  /* Memory fence */

/* Backend types */
#define CXL_BACKEND_TCP   0
#define CXL_BACKEND_SHMEM 1

/* ============================================================================
 * Shared Memory Coherency Protocol
 * ============================================================================
 * Lock-free MESI coherency via shared memory for low-latency communication.
 * Layout: /dev/shm/cxlmemsim_coherency
 */

#define CXL_SHM_COHERENCY_PATH "/dev/shm/cxlmemsim_coherency"
#define CXL_SHM_COHERENCY_MAGIC 0x43584C4D  /* "CXLM" */
#define CXL_SHM_COHERENCY_VERSION 1
#define CXL_SHM_MAX_HOSTS 16
#define CXL_SHM_MAX_CACHELINES (16 * 1024 * 1024)  /* 16M cachelines = 1GB memory */

/* MESI states */
#define CXL_MESI_INVALID   0
#define CXL_MESI_SHARED    1
#define CXL_MESI_EXCLUSIVE 2
#define CXL_MESI_MODIFIED  3

/* Per-cacheline coherency state (8 bytes, atomically accessible) */
typedef struct __attribute__((packed, aligned(8))) {
    uint8_t  state;           /* MESI state */
    uint8_t  owner_id;        /* Current owner host ID (0-15) */
    uint16_t sharers_bitmap;  /* Bitmap of hosts sharing this line */
    uint32_t version;         /* Version counter for ABA prevention */
} CXLCachelineState;

/* Shared memory coherency header */
typedef struct __attribute__((aligned(64))) {
    uint32_t magic;           /* CXL_SHM_COHERENCY_MAGIC */
    uint32_t version;         /* Protocol version */
    uint64_t num_cachelines;  /* Number of cacheline entries */
    uint64_t memory_size;     /* Total memory size being tracked */
    uint8_t  num_hosts;       /* Number of registered hosts */
    uint8_t  reserved[7];

    /* Per-host statistics (cache-line aligned) */
    struct __attribute__((aligned(64))) {
        uint64_t reads;
        uint64_t writes;
        uint64_t invalidations_sent;
        uint64_t invalidations_received;
        uint64_t state_transitions;
        uint64_t reserved[3];
    } host_stats[CXL_SHM_MAX_HOSTS];

    /* Cacheline state array follows header (variable size) */
    /* CXLCachelineState cachelines[num_cachelines]; */
} CXLCoherencyHeader;

/* Get pointer to cacheline state array */
static inline CXLCachelineState* cxl_shm_get_cachelines(CXLCoherencyHeader *hdr) {
    return (CXLCachelineState*)((char*)hdr + sizeof(CXLCoherencyHeader));
}

/* Get cacheline index from address */
static inline uint64_t cxl_shm_addr_to_index(uint64_t addr) {
    return (addr / CACHELINE_SIZE) % CXL_SHM_MAX_CACHELINES;
}

/* ============================================================================
 * PGAS Shared Memory Protocol (for shmem backend)
 * ============================================================================ */

#define CXL_PGAS_SHM_NAME "/cxlmemsim_pgas"
#define CXL_PGAS_MAGIC 0x43584C53484D454D  /* "CXLSHMEM" */
#define CXL_PGAS_MAX_SLOTS 64

/* Request types for PGAS protocol */
#define CXL_PGAS_REQ_NONE       0
#define CXL_PGAS_REQ_READ       1
#define CXL_PGAS_REQ_WRITE      2
#define CXL_PGAS_REQ_ATOMIC_FAA 3
#define CXL_PGAS_REQ_ATOMIC_CAS 4
#define CXL_PGAS_REQ_FENCE      5

/* Response status */
#define CXL_PGAS_RESP_NONE  0
#define CXL_PGAS_RESP_OK    1
#define CXL_PGAS_RESP_ERROR 2

/* PGAS slot for request/response (aligned to 128 bytes) */
typedef struct __attribute__((aligned(128))) {
    volatile uint32_t req_type;      /* Request type */
    volatile uint32_t resp_status;   /* Response status */
    volatile uint64_t addr;          /* Address for operation */
    volatile uint64_t size;          /* Size of operation */
    volatile uint64_t value;         /* Value for atomics */
    volatile uint64_t expected;      /* Expected value for CAS */
    volatile uint64_t latency_ns;    /* Simulated latency */
    volatile uint64_t timestamp;     /* Request timestamp */
    uint8_t data[CACHELINE_SIZE];    /* Data buffer */
    uint8_t padding[64 - 8];         /* Align to 128 bytes */
} CXLPGASSlot;

/* PGAS shared memory header */
typedef struct __attribute__((aligned(64))) {
    uint64_t magic;                  /* Magic number for validation */
    uint32_t version;                /* Protocol version */
    uint32_t num_slots;              /* Number of request slots */
    volatile uint32_t server_ready;  /* Server is ready flag */
    uint32_t reserved;
    uint64_t memory_base;            /* Base address of simulated memory */
    uint64_t memory_size;            /* Size of simulated memory */
    uint8_t padding[64 - 40];        /* Pad header to 64 bytes */
    CXLPGASSlot slots[];             /* Request/response slots (variable) */
} CXLPGASHeader;

typedef struct {
    char host[256];
    int port;
    int socket_fd;
    bool connected;
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t total_atomics;
    uint64_t *hotness_map;
    size_t hotness_map_size;
    pthread_mutex_t lock;

    /* Backend selection */
    int backend_type;  /* CXL_BACKEND_TCP or CXL_BACKEND_SHMEM */

    /* PGAS shared memory backend */
    char pgas_shm_name[256];
    int pgas_shm_fd;
    CXLPGASHeader *pgas_header;
    void *pgas_memory;
    size_t pgas_memory_size;
    int pgas_slot_id;  /* Our assigned slot for requests */
} CXLMemSimContext;

/* Request structure - matching server's ServerRequest */
typedef struct {
    uint8_t op_type;       /* 0=READ, 1=WRITE, 2=GET_SHM_INFO, 3=FAA, 4=CAS, 5=FENCE */
    uint64_t addr;
    uint64_t size;
    uint64_t timestamp;
    uint64_t value;        /* Value for FAA (add value) or CAS (desired value) */
    uint64_t expected;     /* Expected value for CAS operation */
    uint8_t data[CACHELINE_SIZE];
} CXLMemSimRequest;

/* Response structure - matching server's ServerResponse */
typedef struct {
    uint8_t status;
    uint64_t latency_ns;
    uint64_t old_value;    /* Previous value returned by atomic operations */
    uint8_t data[CACHELINE_SIZE];
} CXLMemSimResponse;
typedef uint32_t MemTxResult;

typedef struct MemTxAttrs {
    /* Bus masters which don't specify any attributes will get this
     * (via the MEMTXATTRS_UNSPECIFIED constant), so that we can
     * distinguish "all attributes deliberately clear" from
     * "didn't specify" if necessary.
     */
    unsigned int unspecified : 1;
    /* ARM/AMBA: TrustZone Secure access
     * x86: System Management Mode access
     */
    unsigned int secure : 1;
    /* Memory access is usermode (unprivileged) */
    unsigned int user : 1;
    /*
     * Bus interconnect and peripherals can access anything (memories,
     * devices) by default. By setting the 'memory' bit, bus transaction
     * are restricted to "normal" memories (per the AMBA documentation)
     * versus devices. Access to devices will be logged and rejected
     * (see MEMTX_ACCESS_ERROR).
     */
    unsigned int memory : 1;
    /* Requester ID (for MSI for example) */
    unsigned int requester_id : 16;
    /* Invert endianness for this page */
    unsigned int byte_swap : 1;
    /*
     * The following are target-specific page-table bits.  These are not
     * related to actual memory transactions at all.  However, this structure
     * is part of the tlb_fill interface, cached in the cputlb structure,
     * and has unused bits.  These fields will be read by target-specific
     * helpers using env->iotlb[mmu_idx][tlb_index()].attrs.target_tlb_bitN.
     */
    unsigned int target_tlb_bit0 : 1;
    unsigned int target_tlb_bit1 : 1;
    unsigned int target_tlb_bit2 : 1;
} MemTxAttrs;

/* Initialization functions */
int cxlmemsim_init(const char *host, int port);
int cxlmemsim_init_pgas(const char *shm_name);  /* Initialize with PGAS shared memory backend */
void cxlmemsim_cleanup(void);

/* Memory operations */
MemTxResult cxl_type3_read(void *, long unsigned int, long unsigned int*, unsigned int, MemTxAttrs);
MemTxResult cxl_type3_write(void *d,uint64_t  addr, uint64_t data,
    unsigned size, MemTxAttrs attrs);

/* Atomic operations */
int cxlmemsim_atomic_faa(uint64_t addr, uint64_t add_value, uint64_t *old_value);
int cxlmemsim_atomic_cas(uint64_t addr, uint64_t expected, uint64_t desired, uint64_t *old_value);
void cxlmemsim_fence(void);

/* Statistics */
uint64_t cxlmemsim_get_hotness(uint64_t addr);
void cxlmemsim_dump_hotness_stats(void);

// Back invalidation support for keyboard hook
int cxlmemsim_check_invalidation(uint64_t phys_addr, size_t size, void *data);
void cxlmemsim_register_invalidation(uint64_t phys_addr, void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif