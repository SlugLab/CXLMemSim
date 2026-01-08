/*
 * CXL Backend Abstraction Interface
 * Provides a unified interface for different CXL memory backends:
 *   - SHMEM: Shared memory communication with CXLMemSim server
 *   - DAX: Direct access to CXL memory via /dev/dax devices
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 */

#ifndef CXL_BACKEND_H
#define CXL_BACKEND_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Backend types */
typedef enum {
    CXL_BACKEND_NONE = 0,
    CXL_BACKEND_TCP = 1,       /* Original TCP-based CXLMemSim */
    CXL_BACKEND_SHMEM = 2,     /* Shared memory with CXLMemSim server */
    CXL_BACKEND_DAX = 3        /* Direct /dev/dax access */
} cxl_backend_type_t;

/* Forward declarations */
typedef struct cxl_backend cxl_backend_t;
typedef struct cxl_backend_ops cxl_backend_ops_t;

/* Backend configuration */
typedef struct {
    cxl_backend_type_t type;

    union {
        /* TCP backend config */
        struct {
            char host[256];
            int port;
        } tcp;

        /* Shared memory backend config */
        struct {
            char shm_name[256];      /* POSIX shared memory name (e.g., "/cxlmemsim") */
            size_t shm_size;         /* Size of shared memory region */
            bool is_server;          /* True if this is the server side */
        } shmem;

        /* DAX backend config */
        struct {
            char dax_path[256];      /* Path to DAX device (e.g., "/dev/dax0.0") */
            uint64_t base_offset;    /* Base offset within DAX device */
            size_t region_size;      /* Size of region to map */
        } dax;
    };
} cxl_backend_config_t;

/* Statistics common to all backends */
typedef struct {
    uint64_t reads;
    uint64_t writes;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t total_latency_ns;
    uint64_t avg_latency_ns;
} cxl_backend_stats_t;

/* Backend operations interface */
struct cxl_backend_ops {
    /* Initialize the backend with given config */
    int (*init)(cxl_backend_t* backend, const cxl_backend_config_t* config);

    /* Connect/open the backend */
    int (*connect)(cxl_backend_t* backend);

    /* Disconnect/close the backend */
    void (*disconnect)(cxl_backend_t* backend);

    /* Cleanup and free resources */
    void (*finalize)(cxl_backend_t* backend);

    /* Check if backend is ready */
    bool (*is_ready)(cxl_backend_t* backend);

    /* Read from remote/CXL memory */
    int (*read)(cxl_backend_t* backend, uint64_t addr, void* data,
                size_t size, uint64_t* latency_ns);

    /* Write to remote/CXL memory */
    int (*write)(cxl_backend_t* backend, uint64_t addr, const void* data,
                 size_t size, uint64_t* latency_ns);

    /* Bulk read (multi-cacheline) */
    int (*bulk_read)(cxl_backend_t* backend, uint64_t addr, void* data, size_t size);

    /* Bulk write (multi-cacheline) */
    int (*bulk_write)(cxl_backend_t* backend, uint64_t addr, const void* data, size_t size);

    /* Atomic fetch-and-add */
    int (*atomic_faa)(cxl_backend_t* backend, uint64_t addr,
                      uint64_t value, uint64_t* old_value);

    /* Atomic compare-and-swap */
    int (*atomic_cas)(cxl_backend_t* backend, uint64_t addr,
                      uint64_t expected, uint64_t desired, uint64_t* old_value);

    /* Memory fence */
    void (*fence)(cxl_backend_t* backend);

    /* Cache flush */
    void (*flush)(cxl_backend_t* backend, uint64_t addr, size_t size);

    /* Get statistics */
    void (*get_stats)(cxl_backend_t* backend, cxl_backend_stats_t* stats);

    /* Reset statistics */
    void (*reset_stats)(cxl_backend_t* backend);
};

/* Backend context structure */
struct cxl_backend {
    cxl_backend_type_t type;
    const cxl_backend_ops_t* ops;
    bool connected;
    void* priv;                      /* Backend-specific private data */
    void* lock;                      /* Thread safety (pthread_mutex_t*) */

    /* Common statistics */
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t total_bytes_read;
    uint64_t total_bytes_written;
    uint64_t total_latency_ns;
};

/* ============================================================================
 * Backend Factory Functions
 * ============================================================================ */

/**
 * Create a backend of the specified type
 * @param type     Backend type
 * @param config   Backend configuration
 * @return         Allocated backend or NULL on error
 */
cxl_backend_t* cxl_backend_create(cxl_backend_type_t type,
                                   const cxl_backend_config_t* config);

/**
 * Destroy a backend
 * @param backend  Backend to destroy
 */
void cxl_backend_destroy(cxl_backend_t* backend);

/**
 * Get backend type name as string
 * @param type     Backend type
 * @return         String name
 */
const char* cxl_backend_type_name(cxl_backend_type_t type);

/* ============================================================================
 * Convenience Macros
 * ============================================================================ */

#define CXL_BACKEND_READ(b, addr, data, size, lat) \
    ((b)->ops->read((b), (addr), (data), (size), (lat)))

#define CXL_BACKEND_WRITE(b, addr, data, size, lat) \
    ((b)->ops->write((b), (addr), (data), (size), (lat)))

#define CXL_BACKEND_BULK_READ(b, addr, data, size) \
    ((b)->ops->bulk_read((b), (addr), (data), (size)))

#define CXL_BACKEND_BULK_WRITE(b, addr, data, size) \
    ((b)->ops->bulk_write((b), (addr), (data), (size)))

#define CXL_BACKEND_FENCE(b) \
    ((b)->ops->fence((b)))

#define CXL_BACKEND_FLUSH(b, addr, size) \
    ((b)->ops->flush((b), (addr), (size)))

/* ============================================================================
 * Shared Memory Protocol Structures (for SHMEM backend)
 * ============================================================================ */

#define CXL_SHM_MAGIC 0x43584C53484D454D  /* "CXLSHMEM" */
#define CXL_SHM_VERSION 1
#define CXL_SHM_MAX_SLOTS 64
#define CXL_SHM_CACHELINE_SIZE 64

/* Request types */
#define CXL_SHM_REQ_NONE          0
#define CXL_SHM_REQ_READ          1
#define CXL_SHM_REQ_WRITE         2
#define CXL_SHM_REQ_ATOMIC_FAA    3
#define CXL_SHM_REQ_ATOMIC_CAS    4
#define CXL_SHM_REQ_FENCE         5
#define CXL_SHM_REQ_READ_META     6   /* Read with metadata */
#define CXL_SHM_REQ_WRITE_META    7   /* Write with metadata */
#define CXL_SHM_REQ_GET_META      8   /* Get metadata only */
#define CXL_SHM_REQ_SET_META      9   /* Set metadata only */

/* Response status */
#define CXL_SHM_RESP_NONE     0
#define CXL_SHM_RESP_OK       1
#define CXL_SHM_RESP_ERROR    2

/* MESI Cache States */
#define CXL_CACHE_INVALID     0
#define CXL_CACHE_SHARED      1
#define CXL_CACHE_EXCLUSIVE   2
#define CXL_CACHE_MODIFIED    3

/* Metadata flags */
#define CXL_META_FLAG_DIRTY   0x01
#define CXL_META_FLAG_LOCKED  0x02
#define CXL_META_FLAG_PINNED  0x04

/* Cacheline metadata structure (64 bytes) - compatible with Splash libpgas */
typedef struct {
    uint8_t cache_state;        /* MESI state */
    uint8_t owner_id;           /* Current owner host/thread ID */
    uint16_t sharers_bitmap;    /* Bitmap of hosts/threads sharing this line */
    uint32_t access_count;      /* Number of accesses */
    uint64_t last_access_time;  /* Timestamp of last access */
    uint64_t virtual_addr;      /* Virtual address mapping */
    uint64_t physical_addr;     /* Physical address */
    uint32_t version;           /* Version number for coherency */
    uint8_t flags;              /* Various flags (dirty, locked, etc.) */
    uint8_t reserved[23];       /* Reserved for future use */
} __attribute__((packed)) cxl_cacheline_metadata_t;

/* PGAS Memory Entry: 128 bytes (64 data + 64 metadata) per cacheline */
typedef struct {
    uint8_t data[CXL_SHM_CACHELINE_SIZE];  /* 64 bytes of data */
    cxl_cacheline_metadata_t metadata;      /* 64 bytes of metadata */
} __attribute__((packed, aligned(128))) cxl_pgas_entry_t;

/* Shared memory slot for request/response */
typedef struct {
    volatile uint32_t req_type;      /* Request type */
    volatile uint32_t resp_status;   /* Response status */
    volatile uint64_t addr;          /* Address for operation */
    volatile uint64_t size;          /* Size of operation */
    volatile uint64_t value;         /* Value for atomics */
    volatile uint64_t expected;      /* Expected value for CAS */
    volatile uint64_t latency_ns;    /* Simulated latency */
    volatile uint64_t timestamp;     /* Request timestamp */
    uint8_t data[CXL_SHM_CACHELINE_SIZE];  /* Data buffer (64 bytes) */
    cxl_cacheline_metadata_t metadata;     /* Metadata buffer (64 bytes) */
} __attribute__((aligned(256))) cxl_shm_slot_t;

/* Shared memory header */
typedef struct {
    uint64_t magic;                  /* Magic number for validation */
    uint32_t version;                /* Protocol version */
    uint32_t num_slots;              /* Number of request slots */
    volatile uint32_t server_ready;  /* Server is ready flag */
    uint32_t flags;                  /* Header flags */
    uint64_t memory_base;            /* Base address of simulated memory */
    uint64_t memory_size;            /* Size of simulated memory */
    uint64_t num_cachelines;         /* Number of cachelines (memory_size/64) */
    uint32_t metadata_enabled;       /* 1 if metadata transfer is enabled */
    uint32_t entry_size;             /* Size of each entry (64 or 128 bytes) */
    uint8_t padding[64 - 56];        /* Pad header to 64 bytes */
    cxl_shm_slot_t slots[];          /* Request/response slots */
} __attribute__((aligned(64))) cxl_shm_header_t;

/* Header flags */
#define CXL_SHM_FLAG_METADATA_ENABLED  0x01

/* Size calculation */
#define CXL_SHM_HEADER_SIZE(nslots) \
    (sizeof(cxl_shm_header_t) + (nslots) * sizeof(cxl_shm_slot_t))

#ifdef __cplusplus
}
#endif

#endif /* CXL_BACKEND_H */
