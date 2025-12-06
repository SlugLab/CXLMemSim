#ifndef QEMU_CXL_MEMSIM_H
#define QEMU_CXL_MEMSIM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CACHELINE_SIZE 64
#define CXL_READ_OP 0
#define CXL_WRITE_OP 1

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

typedef struct {
    char host[256];
    int port;
    int socket_fd;
    bool connected;
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t *hotness_map;
    size_t hotness_map_size;
    pthread_mutex_t lock;
} CXLMemSimContext;

typedef struct {
    uint8_t op_type;
    uint64_t addr;
    uint64_t size;
    uint64_t timestamp;
    uint8_t data[CACHELINE_SIZE];
} CXLMemSimRequest;

typedef struct {
    uint8_t status;
    uint64_t latency_ns;
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

int cxlmemsim_init(const char *host, int port);
void cxlmemsim_cleanup(void);

MemTxResult cxl_type3_read(void *, long unsigned int, long unsigned int*, unsigned int, MemTxAttrs);
MemTxResult cxl_type3_write(void *d,uint64_t  addr, uint64_t data,
    unsigned size, MemTxAttrs attrs);

uint64_t cxlmemsim_get_hotness(uint64_t addr);
void cxlmemsim_dump_hotness_stats(void);

// Back invalidation support for keyboard hook
int cxlmemsim_check_invalidation(uint64_t phys_addr, size_t size, void *data);
void cxlmemsim_register_invalidation(uint64_t phys_addr, void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif