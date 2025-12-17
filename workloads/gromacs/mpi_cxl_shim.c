#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mpi.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <execinfo.h>
#include <signal.h>
#include <stdarg.h>

#define CACHELINE_SIZE 64
#define DEFAULT_CXL_SIZE (4UL * 1024 * 1024 * 1024)  // 4GB default
#define CXL_ALIGNMENT 4096
#define SHIM_VERSION "3.0"

// Fixed virtual address for CXL shared memory (all ranks map to same address)
// This enables true pointer sharing without offset conversion
#define CXL_FIXED_ADDR ((void *)0x100000000000ULL)  // 1TB mark

// CXL message queue constants
#define CXL_MAX_RANKS 256
#define CXL_MSG_QUEUE_SIZE 1024
#define CXL_MSG_MAX_INLINE_SIZE 4096
#define CXL_HEADER_SIZE (sizeof(cxl_shm_header_t) + CXL_MAX_RANKS * sizeof(cxl_rank_mailbox_t))

// Remotable pointer - offset from shared memory base
typedef uint64_t cxl_rptr_t;
#define CXL_RPTR_NULL ((cxl_rptr_t)-1)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

// Safe memory copy for CXL memory - avoids AVX-512/SIMD instructions that may crash on CXL
// Uses volatile to prevent compiler from optimizing into SIMD memcpy
static inline void cxl_safe_memcpy(void *dst, const void *src, size_t n) {
    volatile unsigned char *d = (volatile unsigned char *)dst;
    const volatile unsigned char *s = (const volatile unsigned char *)src;

    // Copy 8 bytes at a time for better performance while staying safe
    while (n >= 8) {
        *(volatile uint64_t *)d = *(const volatile uint64_t *)s;
        d += 8;
        s += 8;
        n -= 8;
    }
    // Copy remaining bytes
    while (n > 0) {
        *d++ = *s++;
        n--;
    }
}

// Add color output for better visibility
#define RED     "\x1b[31m"
#define GREEN   "\x1b[32m"
#define YELLOW  "\x1b[33m"
#define BLUE    "\x1b[34m"
#define MAGENTA "\x1b[35m"
#define CYAN    "\x1b[36m"
#define RESET   "\x1b[0m"

// ============================================================================
// CXL Shared Memory Structures for Remotable Pointers
// ============================================================================

// Message state in the queue
typedef enum {
    CXL_MSG_EMPTY = 0,
    CXL_MSG_READY = 1,
    CXL_MSG_READING = 2,
    CXL_MSG_CONSUMED = 3
} cxl_msg_state_t;

// Message descriptor in shared memory
typedef struct __attribute__((aligned(CACHELINE_SIZE))) {
    _Atomic uint32_t state;          // cxl_msg_state_t
    uint32_t source_rank;            // Source rank
    uint32_t tag;                    // MPI tag
    uint32_t count;                  // Element count
    uint64_t datatype_size;          // Size of each element
    cxl_rptr_t data_rptr;            // Remotable pointer to data in shared memory
    uint64_t data_size;              // Total data size in bytes
    uint8_t inline_data[CXL_MSG_MAX_INLINE_SIZE];  // Inline data for small messages
    uint8_t is_inline;               // Whether data is inlined
    uint8_t padding[7];              // Alignment padding
} cxl_msg_t;

// Per-rank mailbox - circular buffer for incoming messages
typedef struct __attribute__((aligned(CACHELINE_SIZE))) {
    _Atomic uint64_t head;           // Next write position (producer)
    _Atomic uint64_t tail;           // Next read position (consumer)
    _Atomic uint32_t active;         // Whether this rank is active
    uint32_t rank;                   // Rank number
    char hostname[64];               // Hostname for debugging
    uint32_t pid;                    // Process ID
    uint32_t reserved;               // Reserved for alignment
    // Total fixed fields: 8+8+4+4+64+4+4 = 96 bytes (1.5 cachelines)
    // Next cacheline starts the message queue
    cxl_msg_t messages[CXL_MSG_QUEUE_SIZE];  // Message queue
} cxl_rank_mailbox_t;

// Shared memory header - at the start of the DAX region
typedef struct __attribute__((aligned(CACHELINE_SIZE))) {
    uint64_t magic;                  // Magic number for validation (8)
    uint32_t version;                // Protocol version (4)
    _Atomic uint32_t num_ranks;      // Number of active ranks (4)
    _Atomic uint64_t alloc_offset;   // Next allocation offset (8)
    uint64_t total_size;             // Total shared memory size (8)
    uint8_t initialized;             // Initialization flag (1)
    uint8_t reserved[7];             // Padding to 8-byte boundary (7)
    // Collective operation synchronization
    _Atomic uint32_t coll_barrier_count;  // Barrier counter (4)
    _Atomic uint32_t coll_barrier_sense;  // Barrier sense flag (4)
    _Atomic uint32_t coll_phase;          // Current collective phase (4)
    uint32_t coll_max_ranks;              // Max ranks for collectives (4)
    cxl_rptr_t coll_data_rptr[CXL_MAX_RANKS];  // Per-rank data pointers for collectives
    // Total: 40 + 16 + 64*8 = 568 bytes
} cxl_shm_header_t;

#define CXL_SHM_MAGIC 0x43584C534D454D00ULL  // "CXLSMEM\0"
#define CXL_SHM_VERSION 1

// ============================================================================
// CXL RMA Window Structures for One-Sided Communication
// ============================================================================

#define CXL_MAX_WINDOWS 64
#define CXL_WIN_MAGIC 0x43584C57494E0000ULL  // "CXLWIN\0\0"

// Window synchronization state
typedef enum {
    CXL_WIN_UNLOCKED = 0,
    CXL_WIN_FENCE_EPOCH = 1,
    CXL_WIN_LOCK_EXCLUSIVE = 2,
    CXL_WIN_LOCK_SHARED = 3,
    CXL_WIN_POST_WAIT = 4,
    CXL_WIN_START_COMPLETE = 5
} cxl_win_sync_state_t;

// Per-rank window info stored in shared memory
typedef struct __attribute__((aligned(CACHELINE_SIZE))) {
    cxl_rptr_t base_rptr;            // Remotable pointer to window base
    uint64_t size;                   // Window size in bytes
    _Atomic uint32_t lock_count;     // Number of shared locks held
    _Atomic uint32_t exclusive_lock; // Exclusive lock holder rank (-1 if none)
    _Atomic uint64_t fence_counter;  // Fence epoch counter
    _Atomic uint32_t sync_state;     // Current synchronization state
    uint32_t owner_rank;             // Rank that owns this window portion
} cxl_win_rank_info_t;

// Window metadata in shared memory (one per window)
typedef struct __attribute__((aligned(CACHELINE_SIZE))) {
    uint64_t magic;                  // Magic number for validation
    uint32_t win_id;                 // Window ID
    _Atomic uint32_t ref_count;      // Reference count
    _Atomic uint64_t global_fence;   // Global fence counter for synchronization
    uint32_t comm_size;              // Size of communicator
    uint32_t disp_unit;              // Displacement unit
    _Atomic uint32_t barrier_count;  // Barrier synchronization counter
    _Atomic uint32_t barrier_sense;  // Barrier sense flag
    cxl_win_rank_info_t ranks[CXL_MAX_RANKS];  // Per-rank info
} cxl_win_shm_t;

// Local window tracking structure
typedef struct cxl_window {
    MPI_Win mpi_win;                 // Original MPI window
    cxl_win_shm_t *shm;              // Shared memory window metadata
    void *local_base;                // Local base address
    size_t local_size;               // Local window size
    int my_rank;                     // Rank in window communicator
    int comm_size;                   // Communicator size
    MPI_Comm comm;                   // Associated communicator
    bool cxl_enabled;                // Whether CXL acceleration is enabled
    uint32_t win_id;                 // Window ID
    struct cxl_window *next;         // Linked list for tracking
} cxl_window_t;

// Collective buffer structure in shared memory
typedef struct __attribute__((aligned(CACHELINE_SIZE))) {
    _Atomic uint64_t ready_mask;     // Bitmask of ranks that have contributed
    _Atomic uint64_t done_mask;      // Bitmask of ranks that have consumed
    _Atomic uint32_t phase;          // Current collective phase
    uint32_t root;                   // Root rank for rooted collectives
    uint64_t count;                  // Element count
    uint64_t datatype_size;          // Size of each element
    cxl_rptr_t data_rptr[CXL_MAX_RANKS];  // Per-rank data pointers
} cxl_collective_t;

typedef struct {
    void *base;
    size_t size;
    size_t used;
    int fd;
    bool initialized;
    pthread_mutex_t lock;
    char *type;  // "dax" or "shm"
    // CXL shared memory structures
    cxl_shm_header_t *header;
    cxl_rank_mailbox_t *mailboxes;
    int my_rank;
    int world_size;
    bool cxl_comm_enabled;
} cxl_mem_t;

typedef struct mem_mapping {
    void *cxl_addr;
    void *orig_addr;
    size_t size;
    int ref_count;
    struct mem_mapping *next;
} mem_mapping_t;

static cxl_mem_t g_cxl = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .initialized = false,
    .header = NULL,
    .mailboxes = NULL,
    .my_rank = -1,
    .world_size = 0,
    .cxl_comm_enabled = false
};

static mem_mapping_t *g_mappings = NULL;
static pthread_mutex_t g_mappings_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_hook_count = 0;
static _Atomic bool g_in_mpi_call = false;

// Window tracking
static cxl_window_t *g_windows = NULL;
static pthread_mutex_t g_windows_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint32_t g_next_win_id = 0;

// Collective operation state
static cxl_collective_t *g_collective = NULL;
static pthread_mutex_t g_collective_lock = PTHREAD_MUTEX_INITIALIZER;

// Function pointers - using typeof for better type safety
static typeof(MPI_Init) *orig_MPI_Init = NULL;
static typeof(MPI_Finalize) *orig_MPI_Finalize = NULL;
static typeof(MPI_Send) *orig_MPI_Send = NULL;
static typeof(MPI_Recv) *orig_MPI_Recv = NULL;
static typeof(MPI_Isend) *orig_MPI_Isend = NULL;
static typeof(MPI_Irecv) *orig_MPI_Irecv = NULL;
static typeof(MPI_Alloc_mem) *orig_MPI_Alloc_mem = NULL;
static typeof(MPI_Free_mem) *orig_MPI_Free_mem = NULL;
static typeof(MPI_Win_allocate) *orig_MPI_Win_allocate = NULL;
static typeof(MPI_Win_allocate_shared) *orig_MPI_Win_allocate_shared = NULL;
static typeof(MPI_Comm_rank) *orig_MPI_Comm_rank = NULL;

// RMA function pointers
static typeof(MPI_Win_create) *orig_MPI_Win_create = NULL;
static typeof(MPI_Win_free) *orig_MPI_Win_free = NULL;
static typeof(MPI_Win_fence) *orig_MPI_Win_fence = NULL;
static typeof(MPI_Put) *orig_MPI_Put = NULL;
static typeof(MPI_Get) *orig_MPI_Get = NULL;
static typeof(MPI_Accumulate) *orig_MPI_Accumulate = NULL;
static typeof(MPI_Win_lock) *orig_MPI_Win_lock = NULL;
static typeof(MPI_Win_unlock) *orig_MPI_Win_unlock = NULL;
static typeof(MPI_Win_flush) *orig_MPI_Win_flush = NULL;

// Collective function pointers
static typeof(MPI_Barrier) *orig_MPI_Barrier = NULL;
static typeof(MPI_Bcast) *orig_MPI_Bcast = NULL;
static typeof(MPI_Reduce) *orig_MPI_Reduce = NULL;
static typeof(MPI_Allreduce) *orig_MPI_Allreduce = NULL;
static typeof(MPI_Allgather) *orig_MPI_Allgather = NULL;
static typeof(MPI_Alltoall) *orig_MPI_Alltoall = NULL;
static typeof(MPI_Gather) *orig_MPI_Gather = NULL;
static typeof(MPI_Scatter) *orig_MPI_Scatter = NULL;

// Debug output function
static void shim_log(const char *level, const char *color, const char *format, ...) {
    if (!getenv("CXL_SHIM_QUIET")) {
        char hostname[256];
        gethostname(hostname, sizeof(hostname));
        
        fprintf(stderr, "%s[CXL_SHIM:%s:%d:%s] ", color, hostname, getpid(), level);
        
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        
        fprintf(stderr, "%s", RESET);
        fflush(stderr);
    }
}

#define LOG_INFO(...)    shim_log("INFO", GREEN, __VA_ARGS__)
#define LOG_WARN(...)    shim_log("WARN", YELLOW, __VA_ARGS__)
#define LOG_ERROR(...)   shim_log("ERROR", RED, __VA_ARGS__)
#define LOG_DEBUG(...)   if(getenv("CXL_SHIM_VERBOSE")) shim_log("DEBUG", CYAN, __VA_ARGS__)
#define LOG_TRACE(...)   if(getenv("CXL_SHIM_TRACE")) shim_log("TRACE", MAGENTA, __VA_ARGS__)

// ============================================================================
// Remotable Pointer Functions
// ============================================================================

// Convert local pointer to remotable pointer (offset from base)
static inline cxl_rptr_t ptr_to_rptr(const void *ptr) {
    if (!ptr || !g_cxl.base) return CXL_RPTR_NULL;
    if ((uintptr_t)ptr < (uintptr_t)g_cxl.base ||
        (uintptr_t)ptr >= (uintptr_t)g_cxl.base + g_cxl.size) {
        return CXL_RPTR_NULL;  // Pointer not in shared region
    }
    return (cxl_rptr_t)((uintptr_t)ptr - (uintptr_t)g_cxl.base);
}

// Convert remotable pointer to local pointer
static inline void *rptr_to_ptr(cxl_rptr_t rptr) {
    if (rptr == CXL_RPTR_NULL || !g_cxl.base) return NULL;
    if (rptr >= g_cxl.size) return NULL;  // Out of bounds
    return (void *)((uintptr_t)g_cxl.base + rptr);
}

// Check if a pointer is in the shared CXL region
static inline bool is_cxl_ptr(const void *ptr) {
    if (!ptr || !g_cxl.base || !g_cxl.initialized) return false;
    return ((uintptr_t)ptr >= (uintptr_t)g_cxl.base &&
            (uintptr_t)ptr < (uintptr_t)g_cxl.base + g_cxl.size);
}

// Signal handler for debugging
static void signal_handler(int sig) {
    void *array[20];
    size_t size;
    
    LOG_ERROR("Caught signal %d\n", sig);
    
    size = backtrace(array, 20);
    fprintf(stderr, "Backtrace:\n");
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    
    exit(1);
}

// Get DAX device size from sysfs
static size_t get_dax_size(const char *dax_path) {
    char sysfs_path[512];
    const char *dev_name = strrchr(dax_path, '/');
    if (!dev_name) dev_name = dax_path;
    else dev_name++;

    snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/dax/devices/%s/size", dev_name);

    FILE *f = fopen(sysfs_path, "r");
    if (!f) {
        LOG_WARN("Cannot read DAX size from %s, using stat\n", sysfs_path);
        return 0;
    }

    unsigned long long size = 0;
    if (fscanf(f, "%llu", &size) != 1) {
        fclose(f);
        return 0;
    }
    fclose(f);

    LOG_DEBUG("DAX device %s size from sysfs: %llu bytes\n", dev_name, size);
    return (size_t)size;
}

// Initialize CXL memory
static void init_cxl_memory(void) {
    if (g_cxl.initialized) return;

    pthread_mutex_lock(&g_cxl.lock);
    if (g_cxl.initialized) {
        pthread_mutex_unlock(&g_cxl.lock);
        return;
    }

    const char *dax_path = getenv("CXL_DAX_PATH");
    const char *cxl_size_str = getenv("CXL_MEM_SIZE");
    size_t cxl_size = cxl_size_str ? strtoull(cxl_size_str, NULL, 0) : DEFAULT_CXL_SIZE;

    if (dax_path && strlen(dax_path) > 0) {
        // Use DAX device - open with O_RDWR for shared access
        g_cxl.fd = open(dax_path, O_RDWR);
        if (g_cxl.fd < 0) {
            LOG_ERROR("Failed to open DAX device %s: %s\n", dax_path, strerror(errno));
            goto use_shm;
        }

        // Try to get DAX size from sysfs first
        cxl_size = get_dax_size(dax_path);
        if (cxl_size == 0) {
            // Fallback to stat
            struct stat st;
            if (fstat(g_cxl.fd, &st) == 0) {
                cxl_size = st.st_size;
            }
            if (cxl_size == 0) {
                // Use default if still 0
                cxl_size = DEFAULT_CXL_SIZE;
            }
        }

        // Try to map at fixed address first for true pointer sharing
        g_cxl.base = mmap(CXL_FIXED_ADDR, cxl_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_FIXED_NOREPLACE, g_cxl.fd, 0);
        if (g_cxl.base == MAP_FAILED) {
            // Fallback: try MAP_FIXED (force the mapping)
            g_cxl.base = mmap(CXL_FIXED_ADDR, cxl_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FIXED, g_cxl.fd, 0);
        }
        if (g_cxl.base == MAP_FAILED) {
            // Final fallback: let kernel choose address
            LOG_WARN("Failed to map at fixed address %p, using kernel-chosen address\n", CXL_FIXED_ADDR);
            g_cxl.base = mmap(NULL, cxl_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_cxl.fd, 0);
        }
        if (g_cxl.base == MAP_FAILED) {
            LOG_ERROR("Failed to map DAX device %s: %s\n", dax_path, strerror(errno));
            close(g_cxl.fd);
            goto use_shm;
        }

        g_cxl.type = "dax";
        LOG_INFO("Mapped DAX device %s: %zu bytes (%zu MB) at %p%s\n",
                 dax_path, cxl_size, cxl_size / (1024*1024), g_cxl.base,
                 g_cxl.base == CXL_FIXED_ADDR ? " (FIXED)" : " (dynamic)");

        // For DAX, we need to coordinate allocation between processes
        // Use first cacheline as allocation counter
        if (getenv("CXL_DAX_RESET")) {
            // Only reset if explicitly requested
            memset(g_cxl.base, 0, CACHELINE_SIZE);
            LOG_INFO("Reset DAX allocation counter\n");
        }

        // DAX allocation starts after first cacheline
        g_cxl.used = CACHELINE_SIZE;

    } else {
use_shm:
        // Use shared memory fallback - create a single shared segment
        const char *shm_name = "/cxlmemsim_mpi_shared";

        // Try to open existing first
        g_cxl.fd = shm_open(shm_name, O_RDWR, 0600);

        if (g_cxl.fd < 0) {
            // Create new
            g_cxl.fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
            if (g_cxl.fd < 0) {
                LOG_ERROR("Failed to create/open shared memory %s: %s\n", shm_name, strerror(errno));
                pthread_mutex_unlock(&g_cxl.lock);
                return;
            }

            if (ftruncate(g_cxl.fd, cxl_size) != 0) {
                LOG_ERROR("Failed to resize shared memory: %s\n", strerror(errno));
                close(g_cxl.fd);
                pthread_mutex_unlock(&g_cxl.lock);
                return;
            }
            LOG_INFO("Created new shared memory segment %s\n", shm_name);
        } else {
            // Get existing size
            struct stat st;
            if (fstat(g_cxl.fd, &st) == 0) {
                cxl_size = st.st_size;
            }
            LOG_INFO("Opened existing shared memory segment %s\n", shm_name);
        }

        // Try to map at fixed address first for true pointer sharing
        g_cxl.base = mmap(CXL_FIXED_ADDR, cxl_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_FIXED_NOREPLACE, g_cxl.fd, 0);
        if (g_cxl.base == MAP_FAILED) {
            // Fallback: try MAP_FIXED (force the mapping)
            g_cxl.base = mmap(CXL_FIXED_ADDR, cxl_size, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_FIXED, g_cxl.fd, 0);
        }
        if (g_cxl.base == MAP_FAILED) {
            // Final fallback: let kernel choose address
            LOG_WARN("Failed to map at fixed address %p, using kernel-chosen address\n", CXL_FIXED_ADDR);
            g_cxl.base = mmap(NULL, cxl_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_cxl.fd, 0);
        }
        if (g_cxl.base == MAP_FAILED) {
            LOG_ERROR("Failed to map shared memory: %s\n", strerror(errno));
            close(g_cxl.fd);
            pthread_mutex_unlock(&g_cxl.lock);
            return;
        }

        g_cxl.type = "shm";
        LOG_INFO("Mapped shared memory %s: %zu bytes (%zu MB) at %p%s\n",
                 shm_name, cxl_size, cxl_size / (1024*1024), g_cxl.base,
                 g_cxl.base == CXL_FIXED_ADDR ? " (FIXED)" : " (dynamic)");

        // For shared memory, also use first cacheline for coordination
        g_cxl.used = CACHELINE_SIZE;
    }

    g_cxl.size = cxl_size;
    g_cxl.initialized = true;

    // Initialize the shared memory header and mailboxes
    g_cxl.header = (cxl_shm_header_t *)g_cxl.base;
    g_cxl.mailboxes = (cxl_rank_mailbox_t *)((char *)g_cxl.base + sizeof(cxl_shm_header_t));

    // Check if we need to initialize (first process or reset requested)
    bool need_init = (g_cxl.header->magic != CXL_SHM_MAGIC) || getenv("CXL_DAX_RESET");

    if (need_init) {
        LOG_INFO("Initializing CXL shared memory structures...\n");

        // Initialize header
        g_cxl.header->magic = CXL_SHM_MAGIC;
        g_cxl.header->version = CXL_SHM_VERSION;
        atomic_store(&g_cxl.header->num_ranks, 0);
        g_cxl.header->total_size = cxl_size;
        g_cxl.header->initialized = 1;

        // Set allocation offset after header and mailboxes
        size_t header_size = sizeof(cxl_shm_header_t) + CXL_MAX_RANKS * sizeof(cxl_rank_mailbox_t);
        header_size = (header_size + CXL_ALIGNMENT - 1) & ~(CXL_ALIGNMENT - 1);
        atomic_store(&g_cxl.header->alloc_offset, header_size);

        // Initialize all mailboxes
        for (int i = 0; i < CXL_MAX_RANKS; i++) {
            atomic_store(&g_cxl.mailboxes[i].head, 0);
            atomic_store(&g_cxl.mailboxes[i].tail, 0);
            atomic_store(&g_cxl.mailboxes[i].active, 0);
            g_cxl.mailboxes[i].rank = i;
            g_cxl.mailboxes[i].pid = 0;
            memset(g_cxl.mailboxes[i].hostname, 0, sizeof(g_cxl.mailboxes[i].hostname));

            // Initialize message slots
            for (int j = 0; j < CXL_MSG_QUEUE_SIZE; j++) {
                atomic_store(&g_cxl.mailboxes[i].messages[j].state, CXL_MSG_EMPTY);
            }
        }

        // Initialize collective operation fields
        atomic_store(&g_cxl.header->coll_barrier_count, 0);
        atomic_store(&g_cxl.header->coll_barrier_sense, 0);
        atomic_store(&g_cxl.header->coll_phase, 0);
        g_cxl.header->coll_max_ranks = CXL_MAX_RANKS;
        for (int i = 0; i < CXL_MAX_RANKS; i++) {
            g_cxl.header->coll_data_rptr[i] = CXL_RPTR_NULL;
        }

        LOG_INFO("CXL shared memory structures initialized (header_size=%zu bytes)\n", header_size);
    } else {
        LOG_INFO("Attaching to existing CXL shared memory (version=%u, ranks=%u)\n",
                 g_cxl.header->version, atomic_load(&g_cxl.header->num_ranks));
    }

    // Update used memory based on header's allocation offset
    g_cxl.used = atomic_load(&g_cxl.header->alloc_offset);

    LOG_INFO("CXL memory initialized: type=%s, size=%zu MB, base=%p, header=%p, mailboxes=%p\n",
             g_cxl.type, cxl_size / (1024*1024), g_cxl.base, g_cxl.header, g_cxl.mailboxes);

    pthread_mutex_unlock(&g_cxl.lock);
}

// Register this rank in the CXL shared memory
static void cxl_register_rank(int rank, int world_size) {
    if (!g_cxl.initialized || !g_cxl.header || rank < 0 || rank >= CXL_MAX_RANKS) {
        LOG_WARN("Cannot register rank %d: CXL not initialized or rank out of range\n", rank);
        return;
    }

    g_cxl.my_rank = rank;
    g_cxl.world_size = world_size;

    // Rank 0 resets collective synchronization state for new run
    if (rank == 0) {
        LOG_INFO("Rank 0 resetting collective synchronization state for new run\n");
        // First, mark as not ready by setting coll_max_ranks to 0
        g_cxl.header->coll_max_ranks = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        // Reset all collective fields
        atomic_store(&g_cxl.header->coll_barrier_count, 0);
        atomic_store(&g_cxl.header->coll_barrier_sense, 0);
        atomic_store(&g_cxl.header->coll_phase, 0);
        for (int i = 0; i < CXL_MAX_RANKS; i++) {
            g_cxl.header->coll_data_rptr[i] = CXL_RPTR_NULL;
        }
        // Reset num_ranks counter for new run
        atomic_store(&g_cxl.header->num_ranks, 0);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        // Now mark as ready by setting coll_max_ranks to world_size
        g_cxl.header->coll_max_ranks = world_size;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    } else {
        // Non-rank-0 processes wait for rank 0 to complete reset
        // by waiting for coll_max_ranks to be set to world_size
        int wait_count = 0;
        while (g_cxl.header->coll_max_ranks != (uint32_t)world_size) {
            __asm__ volatile("pause" ::: "memory");
            if (++wait_count > 10000000) {
                LOG_WARN("Rank %d waiting for rank 0 to reset collective state (coll_max_ranks=%u, expected=%d)\n",
                         rank, g_cxl.header->coll_max_ranks, world_size);
                wait_count = 0;
            }
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    }

    cxl_rank_mailbox_t *my_mailbox = &g_cxl.mailboxes[rank];

    // Set up mailbox for this rank
    my_mailbox->rank = rank;
    my_mailbox->pid = getpid();
    gethostname(my_mailbox->hostname, sizeof(my_mailbox->hostname) - 1);
    atomic_store(&my_mailbox->head, 0);
    atomic_store(&my_mailbox->tail, 0);
    atomic_store(&my_mailbox->active, 1);

    // Increment active rank count
    atomic_fetch_add(&g_cxl.header->num_ranks, 1);

    // Enable CXL communication if CXL_DIRECT env is set or by default for DAX
    g_cxl.cxl_comm_enabled = getenv("CXL_DIRECT") || (strcmp(g_cxl.type, "dax") == 0);

    LOG_INFO("Registered rank %d/%d in CXL shared memory (pid=%d, host=%s, cxl_comm=%s)\n",
             rank, world_size, my_mailbox->pid, my_mailbox->hostname,
             g_cxl.cxl_comm_enabled ? "enabled" : "disabled");
}

// Allocate from CXL memory pool
static void *allocate_cxl_memory(size_t size) {
    if (!g_cxl.initialized) {
        init_cxl_memory();
        if (!g_cxl.initialized) return NULL;
    }

    // Align size
    size = (size + CXL_ALIGNMENT - 1) & ~(CXL_ALIGNMENT - 1);

    // Use atomic operations on the header's allocation offset
    if (g_cxl.header) {
        uint64_t old_offset = atomic_fetch_add(&g_cxl.header->alloc_offset, size);
        uint64_t new_offset = old_offset + size;

        // Check if we have space
        if (new_offset > g_cxl.size) {
            // Roll back
            atomic_fetch_sub(&g_cxl.header->alloc_offset, size);
            LOG_WARN("Out of CXL memory: requested=%zu, available=%zu\n",
                     size, g_cxl.size - old_offset);
            return NULL;
        }

        void *ptr = (char *)g_cxl.base + old_offset;
        g_cxl.used = new_offset;  // Update local view

        LOG_TRACE("Allocated %zu bytes at offset %lu (rptr=0x%lx, total used: %lu/%zu)\n",
                  size, old_offset, ptr_to_rptr(ptr), new_offset, g_cxl.size);

        return ptr;
    } else {
        // Fallback for non-header mode (shouldn't happen)
        pthread_mutex_lock(&g_cxl.lock);

        if (g_cxl.used + size > g_cxl.size) {
            LOG_WARN("Out of CXL memory: requested=%zu, available=%zu\n",
                     size, g_cxl.size - g_cxl.used);
            pthread_mutex_unlock(&g_cxl.lock);
            return NULL;
        }

        void *ptr = (char *)g_cxl.base + g_cxl.used;
        g_cxl.used += size;

        LOG_TRACE("Allocated %zu bytes at offset %zu (total used: %zu/%zu)\n",
                  size, (size_t)((char *)ptr - (char *)g_cxl.base), g_cxl.used, g_cxl.size);

        pthread_mutex_unlock(&g_cxl.lock);

        return ptr;
    }
}

// Allocate from CXL and return remotable pointer
static cxl_rptr_t allocate_cxl_rptr(size_t size) {
    void *ptr = allocate_cxl_memory(size);
    return ptr_to_rptr(ptr);
}

// ============================================================================
// CXL Collective Synchronization
// ============================================================================

// Memory fence to ensure visibility - MPI_Barrier handles cross-node sync
static inline void cxl_flush_range(const void *addr, size_t size) {
    (void)addr;
    (void)size;
    // Use memory fence instead of clflush - MPI_Barrier ensures cross-node visibility
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

// Sense-reversing barrier using CXL shared memory (not used - MPI_Barrier preferred)
// Returns the phase number after the barrier (for collective data coordination)
static uint32_t cxl_collective_barrier(int num_ranks) {
    if (!g_cxl.header || num_ranks <= 0) return 0;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // Get current sense value
    uint32_t my_sense = atomic_load(&g_cxl.header->coll_barrier_sense);

    // Increment barrier count
    uint32_t count = atomic_fetch_add(&g_cxl.header->coll_barrier_count, 1) + 1;

    LOG_TRACE("Barrier: rank=%d, count=%u/%d, sense=%u\n",
              g_cxl.my_rank, count, num_ranks, my_sense);

    if (count == (uint32_t)num_ranks) {
        // Last one in - reset count and flip sense
        atomic_store(&g_cxl.header->coll_barrier_count, 0);
        uint32_t new_phase = atomic_fetch_add(&g_cxl.header->coll_phase, 1) + 1;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        atomic_store(&g_cxl.header->coll_barrier_sense, 1 - my_sense);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        LOG_TRACE("Barrier: rank=%d is LAST, new_phase=%u\n", g_cxl.my_rank, new_phase);
        return new_phase;
    } else {
        // Wait for sense to flip
        int spin_count = 0;
        while (atomic_load(&g_cxl.header->coll_barrier_sense) == my_sense) {
            __asm__ volatile("pause" ::: "memory");
            if (++spin_count % 10000000 == 0) {
                LOG_TRACE("Barrier WAIT: rank=%d, sense=%u\n", g_cxl.my_rank, my_sense);
            }
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        LOG_TRACE("Barrier: rank=%d released\n", g_cxl.my_rank);
        return atomic_load(&g_cxl.header->coll_phase);
    }
}

// Register this rank's buffer for a collective operation
static void cxl_collective_register_buffer(int rank, void *buf) {
    if (!g_cxl.header || rank < 0 || rank >= CXL_MAX_RANKS) return;
    cxl_rptr_t rptr = ptr_to_rptr(buf);
    g_cxl.header->coll_data_rptr[rank] = rptr;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    LOG_TRACE("Registered buffer: rank=%d, buf=%p, rptr=0x%lx\n", rank, buf, (unsigned long)rptr);
}

// Get another rank's buffer for a collective operation
static void *cxl_collective_get_buffer(int rank) {
    if (!g_cxl.header || rank < 0 || rank >= CXL_MAX_RANKS) return NULL;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cxl_rptr_t rptr = g_cxl.header->coll_data_rptr[rank];
    void *ptr = rptr_to_ptr(rptr);
    LOG_TRACE("Get buffer: rank=%d, rptr=0x%lx, ptr=%p\n", rank, (unsigned long)rptr, ptr);
    return ptr;
}

// Clear collective buffer registrations
static void cxl_collective_clear_buffers(int num_ranks) {
    if (!g_cxl.header) return;
    for (int i = 0; i < num_ranks && i < CXL_MAX_RANKS; i++) {
        g_cxl.header->coll_data_rptr[i] = CXL_RPTR_NULL;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

// Mapping management
static void register_mapping(void *cxl_addr, void *orig_addr, size_t size) {
    pthread_mutex_lock(&g_mappings_lock);
    
    mem_mapping_t *mapping = malloc(sizeof(mem_mapping_t));
    mapping->cxl_addr = cxl_addr;
    mapping->orig_addr = orig_addr;
    mapping->size = size;
    mapping->ref_count = 1;
    mapping->next = g_mappings;
    g_mappings = mapping;
    
    LOG_TRACE("Registered mapping: orig=%p -> cxl=%p (size=%zu)\n", orig_addr, cxl_addr, size);
    
    pthread_mutex_unlock(&g_mappings_lock);
}

static void *find_cxl_mapping(const void *orig_addr) {
    pthread_mutex_lock(&g_mappings_lock);
    
    mem_mapping_t *curr = g_mappings;
    while (curr) {
        if (curr->orig_addr == orig_addr) {
            void *cxl_addr = curr->cxl_addr;
            pthread_mutex_unlock(&g_mappings_lock);
            return cxl_addr;
        }
        curr = curr->next;
    }
    
    pthread_mutex_unlock(&g_mappings_lock);
    return NULL;
}

// ============================================================================
// CXL Direct Communication Functions
// ============================================================================

// Check if destination rank is available for CXL communication
static bool cxl_rank_available(int rank) {
    if (!g_cxl.initialized || !g_cxl.mailboxes || rank < 0 || rank >= CXL_MAX_RANKS) {
        return false;
    }
    return atomic_load(&g_cxl.mailboxes[rank].active) != 0;
}

// Send message via CXL shared memory (returns 0 on success, -1 on failure)
static int cxl_send(const void *buf, size_t data_size, int dest, int tag, int source_rank) {
    if (!g_cxl.cxl_comm_enabled || !cxl_rank_available(dest)) {
        return -1;  // Fall back to MPI
    }

    cxl_rank_mailbox_t *dest_mailbox = &g_cxl.mailboxes[dest];

    // Get next slot in destination's queue
    uint64_t head = atomic_load(&dest_mailbox->head);
    uint64_t tail = atomic_load(&dest_mailbox->tail);

    // Check if queue is full
    if ((head - tail) >= CXL_MSG_QUEUE_SIZE) {
        LOG_WARN("CXL send: destination %d queue full (head=%lu, tail=%lu)\n", dest, head, tail);
        return -1;
    }

    uint64_t slot = head % CXL_MSG_QUEUE_SIZE;
    cxl_msg_t *msg = &dest_mailbox->messages[slot];

    // Wait for slot to be empty (with timeout)
    int retries = 0;
    while (atomic_load(&msg->state) != CXL_MSG_EMPTY && retries < 10000) {
        __builtin_ia32_pause();  // CPU hint for spin-wait
        retries++;
    }

    if (atomic_load(&msg->state) != CXL_MSG_EMPTY) {
        LOG_WARN("CXL send: slot %lu not empty after timeout\n", slot);
        return -1;
    }

    // Fill message metadata
    msg->source_rank = source_rank;
    msg->tag = tag;
    msg->data_size = data_size;

    // Decide between inline data and remotable pointer
    if (data_size <= CXL_MSG_MAX_INLINE_SIZE) {
        // Inline small messages (use safe copy for CXL memory)
        cxl_safe_memcpy(msg->inline_data, buf, data_size);
        msg->is_inline = 1;
        msg->data_rptr = CXL_RPTR_NULL;
        LOG_TRACE("CXL send: inlined %zu bytes to rank %d slot %lu\n", data_size, dest, slot);
    } else {
        // Allocate from shared memory and use remotable pointer
        void *cxl_buf = allocate_cxl_memory(data_size);
        if (!cxl_buf) {
            LOG_WARN("CXL send: failed to allocate %zu bytes for large message\n", data_size);
            return -1;
        }
        cxl_safe_memcpy(cxl_buf, buf, data_size);
        msg->data_rptr = ptr_to_rptr(cxl_buf);
        msg->is_inline = 0;
        LOG_TRACE("CXL send: %zu bytes via rptr=0x%lx to rank %d slot %lu\n",
                  data_size, msg->data_rptr, dest, slot);
    }

    // Memory barrier to ensure data is visible before state change
    __atomic_thread_fence(__ATOMIC_RELEASE);

    // Mark message as ready
    atomic_store(&msg->state, CXL_MSG_READY);

    // Advance head pointer
    atomic_fetch_add(&dest_mailbox->head, 1);

    LOG_DEBUG("CXL send: sent %zu bytes from rank %d to rank %d (tag=%d, slot=%lu, inline=%d)\n",
              data_size, source_rank, dest, tag, slot, msg->is_inline);

    return 0;
}

// Receive message via CXL shared memory (returns 0 on success, -1 on failure/no message)
static int cxl_recv(void *buf, size_t max_size, int source, int tag, size_t *actual_size) {
    if (!g_cxl.cxl_comm_enabled || g_cxl.my_rank < 0) {
        return -1;
    }

    cxl_rank_mailbox_t *my_mailbox = &g_cxl.mailboxes[g_cxl.my_rank];

    uint64_t tail = atomic_load(&my_mailbox->tail);
    uint64_t head = atomic_load(&my_mailbox->head);

    // Check if queue is empty
    if (tail >= head) {
        return -1;  // No messages
    }

    // Scan for matching message (support MPI_ANY_SOURCE and MPI_ANY_TAG)
    for (uint64_t i = tail; i < head; i++) {
        uint64_t slot = i % CXL_MSG_QUEUE_SIZE;
        cxl_msg_t *msg = &my_mailbox->messages[slot];

        // Check state
        if (atomic_load(&msg->state) != CXL_MSG_READY) {
            continue;
        }

        // Check source match (MPI_ANY_SOURCE = -1 typically, but we use -2 here to be safe)
        if (source >= 0 && msg->source_rank != (uint32_t)source) {
            continue;
        }

        // Check tag match (MPI_ANY_TAG = -1 typically)
        if (tag >= 0 && msg->tag != (uint32_t)tag) {
            continue;
        }

        // Found matching message - mark as reading
        uint32_t expected = CXL_MSG_READY;
        if (!atomic_compare_exchange_strong(&msg->state, &expected, CXL_MSG_READING)) {
            continue;  // Someone else grabbed it
        }

        // Memory barrier to ensure we see all data
        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        // Copy data (use safe copy for CXL memory)
        size_t copy_size = MIN(msg->data_size, max_size);
        if (msg->is_inline) {
            cxl_safe_memcpy(buf, msg->inline_data, copy_size);
        } else {
            void *src_ptr = rptr_to_ptr(msg->data_rptr);
            if (src_ptr) {
                cxl_safe_memcpy(buf, src_ptr, copy_size);
            } else {
                LOG_ERROR("CXL recv: invalid rptr 0x%lx\n", msg->data_rptr);
                atomic_store(&msg->state, CXL_MSG_READY);  // Put it back
                continue;
            }
        }

        if (actual_size) {
            *actual_size = msg->data_size;
        }

        LOG_DEBUG("CXL recv: received %zu bytes from rank %d (tag=%d, slot=%lu, inline=%d)\n",
                  msg->data_size, msg->source_rank, msg->tag, slot, msg->is_inline);

        // Mark as consumed
        atomic_store(&msg->state, CXL_MSG_CONSUMED);

        // If this was the tail message, advance tail
        if (i == tail) {
            // Advance tail past all consumed messages
            while (tail < head) {
                slot = tail % CXL_MSG_QUEUE_SIZE;
                if (atomic_load(&my_mailbox->messages[slot].state) == CXL_MSG_CONSUMED) {
                    atomic_store(&my_mailbox->messages[slot].state, CXL_MSG_EMPTY);
                    tail++;
                } else {
                    break;
                }
            }
            atomic_store(&my_mailbox->tail, tail);
        }

        return 0;
    }

    return -1;  // No matching message
}

// Blocking CXL receive with spin-wait
static int cxl_recv_blocking(void *buf, size_t max_size, int source, int tag,
                             size_t *actual_size, int timeout_us) {
    int elapsed = 0;
    const int spin_interval = 100;  // microseconds

    while (elapsed < timeout_us) {
        int ret = cxl_recv(buf, max_size, source, tag, actual_size);
        if (ret == 0) {
            return 0;
        }

        // Spin wait
        for (int i = 0; i < 100; i++) {
            __builtin_ia32_pause();
        }
        usleep(spin_interval);
        elapsed += spin_interval;
    }

    return -1;  // Timeout
}

// Load original function
#define LOAD_ORIGINAL(func) \
    do { \
        if (!orig_##func) { \
            orig_##func = dlsym(RTLD_NEXT, #func); \
            if (!orig_##func) { \
                LOG_ERROR("Failed to load original " #func ": %s\n", dlerror()); \
            } else { \
                LOG_TRACE("Loaded original " #func " at %p\n", orig_##func); \
            } \
        } \
    } while(0)

// MPI Function Hooks
int MPI_Init(int *argc, char ***argv) {
    atomic_fetch_add(&g_hook_count, 1);
    LOG_INFO("=== MPI_Init HOOK CALLED (hook #%d) ===\n", g_hook_count);

    LOAD_ORIGINAL(MPI_Init);
    LOAD_ORIGINAL(MPI_Comm_rank);

    // Initialize CXL memory before MPI
    init_cxl_memory();

    LOG_DEBUG("Calling original MPI_Init at %p\n", orig_MPI_Init);
    int ret = orig_MPI_Init(argc, argv);

    if (ret == MPI_SUCCESS) {
        int rank = -1, size = -1;
        if (orig_MPI_Comm_rank) {
            orig_MPI_Comm_rank(MPI_COMM_WORLD, &rank);
            MPI_Comm_size(MPI_COMM_WORLD, &size);
        }

        // Register this rank in CXL shared memory
        if (g_cxl.initialized && rank >= 0) {
            cxl_register_rank(rank, size);
        }

        LOG_INFO("MPI_Init completed: rank=%d/%d, CXL=%s, CXL_DIRECT=%s\n",
                 rank, size,
                 g_cxl.initialized ? "initialized" : "not initialized",
                 g_cxl.cxl_comm_enabled ? "enabled" : "disabled");
    } else {
        LOG_ERROR("MPI_Init failed with code %d\n", ret);
    }

    return ret;
}

int MPI_Finalize(void) {
    LOG_INFO("=== MPI_Finalize HOOK CALLED ===\n");

    LOAD_ORIGINAL(MPI_Finalize);

    // Deactivate this rank in CXL shared memory before MPI finalize
    if (g_cxl.initialized && g_cxl.mailboxes && g_cxl.my_rank >= 0) {
        atomic_store(&g_cxl.mailboxes[g_cxl.my_rank].active, 0);
        atomic_fetch_sub(&g_cxl.header->num_ranks, 1);
        LOG_INFO("Deactivated rank %d in CXL shared memory\n", g_cxl.my_rank);
    }

    int ret = orig_MPI_Finalize();

    // Cleanup CXL memory
    if (g_cxl.initialized) {
        LOG_INFO("Cleaning up CXL memory (used %zu/%zu bytes, %.1f%%)\n",
                 g_cxl.used, g_cxl.size, 100.0 * g_cxl.used / g_cxl.size);
        munmap(g_cxl.base, g_cxl.size);
        close(g_cxl.fd);

        // Don't unlink shared memory as other processes may still be using it
        // Only unlink if explicitly requested
        if (strcmp(g_cxl.type, "shm") == 0 && getenv("CXL_SHM_UNLINK")) {
            const char *shm_name = "/cxlmemsim_mpi_shared";
            shm_unlink(shm_name);
            LOG_INFO("Unlinked shared memory %s\n", shm_name);
        }

        g_cxl.initialized = false;
        g_cxl.cxl_comm_enabled = false;
        g_cxl.my_rank = -1;
    }

    LOG_INFO("MPI_Finalize completed (total hooks: %d)\n", g_hook_count);

    return ret;
}

int MPI_Send(const void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm) {
    static _Atomic int send_count = 0;
    static _Atomic int cxl_send_count = 0;
    int call_num = atomic_fetch_add(&send_count, 1);

    int type_size;
    MPI_Type_size(datatype, &type_size);
    size_t total_size = (size_t)count * type_size;

    LOG_DEBUG("MPI_Send[%d]: count=%d, dest=%d, tag=%d, buf=%p, size=%zu\n",
              call_num, count, dest, tag, buf, total_size);

    LOAD_ORIGINAL(MPI_Send);

    // Try CXL direct communication first if enabled and destination is available
    if (g_cxl.cxl_comm_enabled && cxl_rank_available(dest)) {
        int cxl_ret = cxl_send(buf, total_size, dest, tag, g_cxl.my_rank);
        if (cxl_ret == 0) {
            int cxl_num = atomic_fetch_add(&cxl_send_count, 1);
            LOG_DEBUG("MPI_Send[%d]: CXL direct send #%d successful (%zu bytes to rank %d)\n",
                      call_num, cxl_num, total_size, dest);
            return MPI_SUCCESS;
        }
        LOG_TRACE("MPI_Send[%d]: CXL direct send failed, falling back to MPI\n", call_num);
    }

    // Fallback to original MPI_Send
    void *send_buf = (void *)buf;

    // Optional: copy to CXL memory for the MPI path (for memory placement experiments)
    if (g_cxl.initialized && getenv("CXL_SHIM_COPY_SEND")) {
        void *cxl_buf = allocate_cxl_memory(total_size);
        if (cxl_buf) {
            cxl_safe_memcpy(cxl_buf, buf, total_size);
            send_buf = cxl_buf;
            LOG_TRACE("MPI_Send[%d]: copied %zu bytes to CXL at %p (rptr=0x%lx)\n",
                      call_num, total_size, cxl_buf, ptr_to_rptr(cxl_buf));
        }
    }

    return orig_MPI_Send(send_buf, count, datatype, dest, tag, comm);
}

int MPI_Recv(void *buf, int count, MPI_Datatype datatype, int source, int tag,
             MPI_Comm comm, MPI_Status *status) {
    static _Atomic int recv_count = 0;
    static _Atomic int cxl_recv_count = 0;
    int call_num = atomic_fetch_add(&recv_count, 1);

    int type_size;
    MPI_Type_size(datatype, &type_size);
    size_t max_size = (size_t)count * type_size;

    LOG_DEBUG("MPI_Recv[%d]: count=%d, source=%d, tag=%d, buf=%p, max_size=%zu\n",
              call_num, count, source, tag, buf, max_size);

    LOAD_ORIGINAL(MPI_Recv);

    // Try CXL direct communication first if enabled
    // Note: For MPI_ANY_SOURCE (-1) or MPI_ANY_TAG (-1), we still try CXL first
    if (g_cxl.cxl_comm_enabled && g_cxl.my_rank >= 0) {
        // For blocking receive, use a timeout-based approach
        // If CXL has a message, use it; otherwise fall back to MPI
        size_t actual_size = 0;

        // First try non-blocking check for CXL message
        int cxl_ret = cxl_recv(buf, max_size, source, tag, &actual_size);
        if (cxl_ret == 0) {
            int cxl_num = atomic_fetch_add(&cxl_recv_count, 1);
            LOG_DEBUG("MPI_Recv[%d]: CXL direct recv #%d successful (%zu bytes from source %d)\n",
                      call_num, cxl_num, actual_size, source);

            // Fill in status if provided
            if (status && status != MPI_STATUS_IGNORE) {
                // Note: We need to find actual source from the message
                // For simplicity, set count based on actual_size
                status->MPI_SOURCE = source >= 0 ? source : 0;  // TODO: get actual source
                status->MPI_TAG = tag >= 0 ? tag : 0;           // TODO: get actual tag
                status->MPI_ERROR = MPI_SUCCESS;
            }
            return MPI_SUCCESS;
        }
        LOG_TRACE("MPI_Recv[%d]: No CXL message available, using MPI\n", call_num);
    }

    // Fallback to original MPI_Recv
    void *recv_buf = buf;
    void *cxl_buf = NULL;

    // Optional: use CXL memory for receive buffer (for memory placement experiments)
    if (g_cxl.initialized && getenv("CXL_SHIM_COPY_RECV")) {
        cxl_buf = allocate_cxl_memory(max_size);
        if (cxl_buf) {
            recv_buf = cxl_buf;
            LOG_TRACE("MPI_Recv[%d]: using CXL buffer at %p (rptr=0x%lx, size=%zu)\n",
                      call_num, cxl_buf, ptr_to_rptr(cxl_buf), max_size);
        }
    }

    int ret = orig_MPI_Recv(recv_buf, count, datatype, source, tag, comm, status);

    if (cxl_buf && ret == MPI_SUCCESS) {
        cxl_safe_memcpy(buf, cxl_buf, max_size);
        LOG_TRACE("MPI_Recv[%d]: copied %zu bytes from CXL\n", call_num, max_size);
    }

    return ret;
}

// Non-blocking send with CXL support
int MPI_Isend(const void *buf, int count, MPI_Datatype datatype, int dest, int tag,
              MPI_Comm comm, MPI_Request *request) {
    static _Atomic int isend_count = 0;
    static _Atomic int cxl_isend_count = 0;
    int call_num = atomic_fetch_add(&isend_count, 1);

    int type_size;
    MPI_Type_size(datatype, &type_size);
    size_t total_size = (size_t)count * type_size;

    LOG_DEBUG("MPI_Isend[%d]: count=%d, dest=%d, tag=%d, buf=%p, size=%zu\n",
              call_num, count, dest, tag, buf, total_size);

    LOAD_ORIGINAL(MPI_Isend);

    // Try CXL direct communication for non-blocking send
    // Since CXL send is already non-blocking (writes to shared memory), we can use it directly
    if (g_cxl.cxl_comm_enabled && cxl_rank_available(dest)) {
        int cxl_ret = cxl_send(buf, total_size, dest, tag, g_cxl.my_rank);
        if (cxl_ret == 0) {
            int cxl_num = atomic_fetch_add(&cxl_isend_count, 1);
            LOG_DEBUG("MPI_Isend[%d]: CXL direct send #%d successful (%zu bytes to rank %d)\n",
                      call_num, cxl_num, total_size, dest);

            // For CXL path, we complete immediately - create a null request
            // that will complete instantly on MPI_Wait/MPI_Test
            *request = MPI_REQUEST_NULL;
            return MPI_SUCCESS;
        }
        LOG_TRACE("MPI_Isend[%d]: CXL direct send failed, falling back to MPI\n", call_num);
    }

    // Fallback to original MPI_Isend
    void *send_buf = (void *)buf;

    if (g_cxl.initialized && getenv("CXL_SHIM_COPY_SEND")) {
        void *cxl_buf = allocate_cxl_memory(total_size);
        if (cxl_buf) {
            cxl_safe_memcpy(cxl_buf, buf, total_size);
            send_buf = cxl_buf;
            LOG_TRACE("MPI_Isend[%d]: copied %zu bytes to CXL at %p\n",
                      call_num, total_size, cxl_buf);
        }
    }

    return orig_MPI_Isend(send_buf, count, datatype, dest, tag, comm, request);
}

// Non-blocking receive with CXL support
int MPI_Irecv(void *buf, int count, MPI_Datatype datatype, int source, int tag,
              MPI_Comm comm, MPI_Request *request) {
    static _Atomic int irecv_count = 0;
    static _Atomic int cxl_irecv_count = 0;
    int call_num = atomic_fetch_add(&irecv_count, 1);

    int type_size;
    MPI_Type_size(datatype, &type_size);
    size_t max_size = (size_t)count * type_size;

    LOG_DEBUG("MPI_Irecv[%d]: count=%d, source=%d, tag=%d, buf=%p, max_size=%zu\n",
              call_num, count, source, tag, buf, max_size);

    LOAD_ORIGINAL(MPI_Irecv);

    // Try CXL direct receive - check if a message is already available
    if (g_cxl.cxl_comm_enabled && g_cxl.my_rank >= 0) {
        size_t actual_size = 0;
        int cxl_ret = cxl_recv(buf, max_size, source, tag, &actual_size);
        if (cxl_ret == 0) {
            int cxl_num = atomic_fetch_add(&cxl_irecv_count, 1);
            LOG_DEBUG("MPI_Irecv[%d]: CXL direct recv #%d already available (%zu bytes)\n",
                      call_num, cxl_num, actual_size);

            // Message already received - return null request
            *request = MPI_REQUEST_NULL;
            return MPI_SUCCESS;
        }
        LOG_TRACE("MPI_Irecv[%d]: No CXL message available, using MPI\n", call_num);
    }

    // Fallback to original MPI_Irecv
    return orig_MPI_Irecv(buf, count, datatype, source, tag, comm, request);
}

// Sendrecv with CXL optimization
int MPI_Sendrecv(const void *sendbuf, int sendcount, MPI_Datatype sendtype, int dest, int sendtag,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype, int source, int recvtag,
                 MPI_Comm comm, MPI_Status *status) {
    static _Atomic int sendrecv_count = 0;
    int call_num = atomic_fetch_add(&sendrecv_count, 1);

    LOG_DEBUG("MPI_Sendrecv[%d]: dest=%d, source=%d\n", call_num, dest, source);

    // For sendrecv, try CXL for both operations
    int send_size, recv_size;
    MPI_Type_size(sendtype, &send_size);
    MPI_Type_size(recvtype, &recv_size);
    size_t send_total = (size_t)sendcount * send_size;
    size_t recv_total = (size_t)recvcount * recv_size;

    bool cxl_send_ok = false;
    bool cxl_recv_ok = false;

    // Try CXL send
    if (g_cxl.cxl_comm_enabled && cxl_rank_available(dest)) {
        if (cxl_send(sendbuf, send_total, dest, sendtag, g_cxl.my_rank) == 0) {
            cxl_send_ok = true;
            LOG_TRACE("MPI_Sendrecv[%d]: CXL send successful\n", call_num);
        }
    }

    // Try CXL recv
    if (g_cxl.cxl_comm_enabled && g_cxl.my_rank >= 0) {
        size_t actual_size = 0;
        // Use blocking receive with short timeout for sendrecv
        if (cxl_recv_blocking(recvbuf, recv_total, source, recvtag, &actual_size, 1000) == 0) {
            cxl_recv_ok = true;
            LOG_TRACE("MPI_Sendrecv[%d]: CXL recv successful\n", call_num);
        }
    }

    // If both succeeded via CXL, we're done
    if (cxl_send_ok && cxl_recv_ok) {
        if (status && status != MPI_STATUS_IGNORE) {
            status->MPI_SOURCE = source;
            status->MPI_TAG = recvtag;
            status->MPI_ERROR = MPI_SUCCESS;
        }
        return MPI_SUCCESS;
    }

    // Otherwise, use original MPI_Sendrecv
    // Note: This may result in duplicate messages if CXL partially succeeded
    // For safety, we fall back to MPI for the full operation
    static typeof(MPI_Sendrecv) *orig_MPI_Sendrecv = NULL;
    if (!orig_MPI_Sendrecv) {
        orig_MPI_Sendrecv = dlsym(RTLD_NEXT, "MPI_Sendrecv");
    }

    if (orig_MPI_Sendrecv) {
        return orig_MPI_Sendrecv(sendbuf, sendcount, sendtype, dest, sendtag,
                                  recvbuf, recvcount, recvtype, source, recvtag,
                                  comm, status);
    }

    LOG_ERROR("MPI_Sendrecv: original function not found\n");
    return MPI_ERR_INTERN;
}

// ============================================================================
// CXL Window Management Functions
// ============================================================================

// Find window by MPI_Win handle
static cxl_window_t *find_cxl_window(MPI_Win win) {
    pthread_mutex_lock(&g_windows_lock);
    cxl_window_t *curr = g_windows;
    while (curr) {
        if (curr->mpi_win == win) {
            pthread_mutex_unlock(&g_windows_lock);
            return curr;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_windows_lock);
    return NULL;
}

// Register a new CXL window
static cxl_window_t *register_cxl_window(MPI_Win win, void *base, size_t size,
                                          int rank, int comm_size, MPI_Comm comm) {
    cxl_window_t *cxl_win = malloc(sizeof(cxl_window_t));
    if (!cxl_win) return NULL;

    cxl_win->mpi_win = win;
    cxl_win->local_base = base;
    cxl_win->local_size = size;
    cxl_win->my_rank = rank;
    cxl_win->comm_size = comm_size;
    cxl_win->comm = comm;
    cxl_win->win_id = atomic_fetch_add(&g_next_win_id, 1);
    cxl_win->cxl_enabled = false;
    cxl_win->shm = NULL;

    // Allocate shared memory for window metadata if CXL is available
    if (g_cxl.initialized && g_cxl.cxl_comm_enabled) {
        LOAD_ORIGINAL(MPI_Bcast);
        LOAD_ORIGINAL(MPI_Barrier);

        // Only rank 0 allocates the shared window metadata structure
        // Then broadcast the rptr to all ranks so they share the same structure
        cxl_rptr_t shm_rptr = CXL_RPTR_NULL;

        if (rank == 0) {
            cxl_win->shm = (cxl_win_shm_t *)allocate_cxl_memory(sizeof(cxl_win_shm_t));
            if (cxl_win->shm) {
                shm_rptr = ptr_to_rptr(cxl_win->shm);
                // Initialize the shared structure
                cxl_win->shm->magic = CXL_WIN_MAGIC;
                cxl_win->shm->win_id = cxl_win->win_id;
                atomic_store(&cxl_win->shm->ref_count, 0);
                atomic_store(&cxl_win->shm->global_fence, 0);
                cxl_win->shm->comm_size = comm_size;
                cxl_win->shm->disp_unit = 1;
                atomic_store(&cxl_win->shm->barrier_count, 0);
                atomic_store(&cxl_win->shm->barrier_sense, 0);
                // Initialize all rank info entries
                for (int r = 0; r < comm_size && r < CXL_MAX_RANKS; r++) {
                    cxl_win->shm->ranks[r].base_rptr = CXL_RPTR_NULL;
                    cxl_win->shm->ranks[r].size = 0;
                    cxl_win->shm->ranks[r].owner_rank = r;
                    atomic_store(&cxl_win->shm->ranks[r].lock_count, 0);
                    atomic_store(&cxl_win->shm->ranks[r].exclusive_lock, (uint32_t)-1);
                    atomic_store(&cxl_win->shm->ranks[r].fence_counter, 0);
                    atomic_store(&cxl_win->shm->ranks[r].sync_state, CXL_WIN_UNLOCKED);
                }
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
            }
        }

        // Broadcast the shm rptr from rank 0 to all ranks
        orig_MPI_Bcast(&shm_rptr, sizeof(shm_rptr), MPI_BYTE, 0, comm);

        // All ranks convert rptr to local pointer
        if (shm_rptr != CXL_RPTR_NULL) {
            cxl_win->shm = (cxl_win_shm_t *)rptr_to_ptr(shm_rptr);
        }

        if (cxl_win->shm) {
            // Now all ranks share the same shm structure
            // Register this rank's window region
            cxl_win_rank_info_t *rank_info = &cxl_win->shm->ranks[rank];

            // Only use CXL acceleration if base is already in CXL memory
            // Copying non-CXL buffers would break MPI semantics since
            // Put/Get/Accumulate would modify the copy, not the original
            if (is_cxl_ptr(base)) {
                rank_info->base_rptr = ptr_to_rptr(base);
                rank_info->size = size;
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                atomic_fetch_add(&cxl_win->shm->ref_count, 1);
                cxl_win->cxl_enabled = true;
            } else {
                // Non-CXL buffer - disable CXL acceleration for this window
                rank_info->base_rptr = CXL_RPTR_NULL;
                rank_info->size = 0;
                cxl_win->cxl_enabled = false;
                LOG_DEBUG("Window base %p is not in CXL memory, disabling CXL acceleration\n", base);
            }

            // Barrier to ensure all ranks have registered before proceeding
            orig_MPI_Barrier(comm);

            LOG_DEBUG("Registered CXL window %u for rank %d: shm_rptr=0x%lx, base_rptr=0x%lx, size=%zu\n",
                      cxl_win->win_id, rank, (unsigned long)shm_rptr,
                      (unsigned long)rank_info->base_rptr, size);
        }
    }

    // Add to global list
    pthread_mutex_lock(&g_windows_lock);
    cxl_win->next = g_windows;
    g_windows = cxl_win;
    pthread_mutex_unlock(&g_windows_lock);

    return cxl_win;
}

// Unregister a CXL window
static void unregister_cxl_window(MPI_Win win) {
    pthread_mutex_lock(&g_windows_lock);
    cxl_window_t **curr = &g_windows;
    while (*curr) {
        if ((*curr)->mpi_win == win) {
            cxl_window_t *to_free = *curr;
            *curr = (*curr)->next;

            if (to_free->shm) {
                atomic_fetch_sub(&to_free->shm->ref_count, 1);
            }

            free(to_free);
            pthread_mutex_unlock(&g_windows_lock);
            return;
        }
        curr = &(*curr)->next;
    }
    pthread_mutex_unlock(&g_windows_lock);
}

// CXL barrier implementation using shared memory
static void cxl_barrier(cxl_win_shm_t *shm, int rank, int comm_size) {
    uint32_t sense = atomic_load(&shm->barrier_sense);
    uint32_t count = atomic_fetch_add(&shm->barrier_count, 1) + 1;

    if (count == (uint32_t)comm_size) {
        // Last to arrive - reset and flip sense
        atomic_store(&shm->barrier_count, 0);
        atomic_store(&shm->barrier_sense, sense ^ 1);
    } else {
        // Wait for sense to flip
        while (atomic_load(&shm->barrier_sense) == sense) {
            __builtin_ia32_pause();
        }
    }
}

// ============================================================================
// MPI Window Operations with CXL Acceleration
// ============================================================================

int MPI_Win_create(void *base, MPI_Aint size, int disp_unit, MPI_Info info,
                   MPI_Comm comm, MPI_Win *win) {
    LOG_DEBUG("MPI_Win_create: base=%p, size=%ld, disp_unit=%d\n", base, (long)size, disp_unit);

    LOAD_ORIGINAL(MPI_Win_create);

    int ret = orig_MPI_Win_create(base, size, disp_unit, info, comm, win);

    if (ret == MPI_SUCCESS && g_cxl.initialized) {
        int rank, comm_size;
        MPI_Comm_rank(comm, &rank);
        MPI_Comm_size(comm, &comm_size);

        cxl_window_t *cxl_win = register_cxl_window(*win, base, size, rank, comm_size, comm);
        if (cxl_win && cxl_win->shm) {
            cxl_win->shm->disp_unit = disp_unit;
        }

        LOG_INFO("MPI_Win_create: created window %p with CXL=%s\n",
                 (void *)*win, (cxl_win && cxl_win->cxl_enabled) ? "enabled" : "disabled");
    }

    return ret;
}

int MPI_Win_allocate(MPI_Aint size, int disp_unit, MPI_Info info,
                     MPI_Comm comm, void *baseptr, MPI_Win *win) {
    LOG_DEBUG("MPI_Win_allocate: size=%ld, disp_unit=%d\n", (long)size, disp_unit);

    // Try to allocate from CXL memory first
    void *cxl_base = NULL;
    if (g_cxl.initialized && g_cxl.cxl_comm_enabled) {
        cxl_base = allocate_cxl_memory(size);
        if (cxl_base) {
            LOG_DEBUG("MPI_Win_allocate: using CXL memory at %p (rptr=0x%lx)\n",
                      cxl_base, ptr_to_rptr(cxl_base));
        }
    }

    LOAD_ORIGINAL(MPI_Win_allocate);

    int ret;
    if (cxl_base) {
        // Use MPI_Win_create with our CXL memory
        LOAD_ORIGINAL(MPI_Win_create);
        ret = orig_MPI_Win_create(cxl_base, size, disp_unit, info, comm, win);
        *(void **)baseptr = cxl_base;
    } else {
        ret = orig_MPI_Win_allocate(size, disp_unit, info, comm, baseptr, win);
    }

    if (ret == MPI_SUCCESS) {
        int rank, comm_size;
        MPI_Comm_rank(comm, &rank);
        MPI_Comm_size(comm, &comm_size);

        cxl_window_t *cxl_win = register_cxl_window(*win, *(void **)baseptr, size,
                                                     rank, comm_size, comm);
        if (cxl_win && cxl_win->shm) {
            cxl_win->shm->disp_unit = disp_unit;
        }

        LOG_INFO("MPI_Win_allocate: allocated window %p, base=%p, CXL=%s\n",
                 (void *)*win, *(void **)baseptr,
                 (cxl_win && cxl_win->cxl_enabled) ? "enabled" : "disabled");
    }

    return ret;
}

int MPI_Win_free(MPI_Win *win) {
    LOG_DEBUG("MPI_Win_free: win=%p\n", (void *)*win);

    unregister_cxl_window(*win);

    LOAD_ORIGINAL(MPI_Win_free);
    return orig_MPI_Win_free(win);
}

// ============================================================================
// MPI One-Sided RMA Operations with CXL Direct Memory Access
// ============================================================================

int MPI_Put(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype,
            int target_rank, MPI_Aint target_disp, int target_count,
            MPI_Datatype target_datatype, MPI_Win win) {
    static _Atomic int put_count = 0;
    static _Atomic int cxl_put_count = 0;
    int call_num = atomic_fetch_add(&put_count, 1);

    LOG_DEBUG("MPI_Put[%d]: target_rank=%d, target_disp=%ld\n",
              call_num, target_rank, (long)target_disp);

    LOAD_ORIGINAL(MPI_Put);

    cxl_window_t *cxl_win = find_cxl_window(win);

    // Try CXL direct put
    if (cxl_win && cxl_win->cxl_enabled && cxl_win->shm) {
        cxl_win_rank_info_t *target_info = &cxl_win->shm->ranks[target_rank];

        if (target_info->base_rptr != CXL_RPTR_NULL) {
            void *target_base = rptr_to_ptr(target_info->base_rptr);
            if (target_base) {
                int origin_size, target_size;
                MPI_Type_size(origin_datatype, &origin_size);
                MPI_Type_size(target_datatype, &target_size);

                size_t origin_bytes = (size_t)origin_count * origin_size;
                void *target_addr = (char *)target_base + target_disp * cxl_win->shm->disp_unit;

                // Direct memory copy via CXL (use safe copy to avoid AVX-512 issues)
                cxl_safe_memcpy(target_addr, origin_addr, origin_bytes);

                // Memory fence to ensure visibility
                __atomic_thread_fence(__ATOMIC_RELEASE);

                int cxl_num = atomic_fetch_add(&cxl_put_count, 1);
                LOG_DEBUG("MPI_Put[%d]: CXL direct put #%d (%zu bytes to rank %d @ offset %ld)\n",
                          call_num, cxl_num, origin_bytes, target_rank, (long)target_disp);

                return MPI_SUCCESS;
            }
        }
    }

    // Fallback to MPI
    return orig_MPI_Put(origin_addr, origin_count, origin_datatype,
                        target_rank, target_disp, target_count,
                        target_datatype, win);
}

int MPI_Get(void *origin_addr, int origin_count, MPI_Datatype origin_datatype,
            int target_rank, MPI_Aint target_disp, int target_count,
            MPI_Datatype target_datatype, MPI_Win win) {
    static _Atomic int get_count = 0;
    static _Atomic int cxl_get_count = 0;
    int call_num = atomic_fetch_add(&get_count, 1);

    LOG_DEBUG("MPI_Get[%d]: target_rank=%d, target_disp=%ld\n",
              call_num, target_rank, (long)target_disp);

    LOAD_ORIGINAL(MPI_Get);

    cxl_window_t *cxl_win = find_cxl_window(win);

    // Try CXL direct get
    if (cxl_win && cxl_win->cxl_enabled && cxl_win->shm) {
        cxl_win_rank_info_t *target_info = &cxl_win->shm->ranks[target_rank];

        if (target_info->base_rptr != CXL_RPTR_NULL) {
            void *target_base = rptr_to_ptr(target_info->base_rptr);
            if (target_base) {
                int origin_size, target_size;
                MPI_Type_size(origin_datatype, &origin_size);
                MPI_Type_size(target_datatype, &target_size);

                size_t origin_bytes = (size_t)origin_count * origin_size;
                void *target_addr = (char *)target_base + target_disp * cxl_win->shm->disp_unit;

                // Memory fence before reading
                __atomic_thread_fence(__ATOMIC_ACQUIRE);

                // Direct memory copy via CXL (use safe copy to avoid AVX-512 issues)
                cxl_safe_memcpy(origin_addr, target_addr, origin_bytes);

                int cxl_num = atomic_fetch_add(&cxl_get_count, 1);
                LOG_DEBUG("MPI_Get[%d]: CXL direct get #%d (%zu bytes from rank %d @ offset %ld)\n",
                          call_num, cxl_num, origin_bytes, target_rank, (long)target_disp);

                return MPI_SUCCESS;
            }
        }
    }

    // Fallback to MPI
    return orig_MPI_Get(origin_addr, origin_count, origin_datatype,
                        target_rank, target_disp, target_count,
                        target_datatype, win);
}

int MPI_Accumulate(const void *origin_addr, int origin_count, MPI_Datatype origin_datatype,
                   int target_rank, MPI_Aint target_disp, int target_count,
                   MPI_Datatype target_datatype, MPI_Op op, MPI_Win win) {
    static _Atomic int acc_count = 0;
    int call_num = atomic_fetch_add(&acc_count, 1);

    LOG_DEBUG("MPI_Accumulate[%d]: target_rank=%d, target_disp=%ld\n",
              call_num, target_rank, (long)target_disp);

    LOAD_ORIGINAL(MPI_Accumulate);

    cxl_window_t *cxl_win = find_cxl_window(win);

    // Try CXL direct accumulate for simple operations
    if (cxl_win && cxl_win->cxl_enabled && cxl_win->shm && op == MPI_SUM) {
        cxl_win_rank_info_t *target_info = &cxl_win->shm->ranks[target_rank];

        if (target_info->base_rptr != CXL_RPTR_NULL) {
            void *target_base = rptr_to_ptr(target_info->base_rptr);
            if (target_base) {
                int type_size;
                MPI_Type_size(origin_datatype, &type_size);
                void *target_addr = (char *)target_base + target_disp * cxl_win->shm->disp_unit;

                // Simple accumulate for common types
                if (origin_datatype == MPI_DOUBLE) {
                    double *src = (double *)origin_addr;
                    double *dst = (double *)target_addr;
                    for (int i = 0; i < origin_count; i++) {
                        __atomic_fetch_add((long long *)&dst[i],
                                           *(long long *)&src[i], __ATOMIC_RELAXED);
                    }
                    LOG_DEBUG("MPI_Accumulate[%d]: CXL direct accumulate (MPI_SUM, double)\n", call_num);
                    return MPI_SUCCESS;
                } else if (origin_datatype == MPI_INT) {
                    int *src = (int *)origin_addr;
                    _Atomic int *dst = (_Atomic int *)target_addr;
                    for (int i = 0; i < origin_count; i++) {
                        atomic_fetch_add(&dst[i], src[i]);
                    }
                    LOG_DEBUG("MPI_Accumulate[%d]: CXL direct accumulate (MPI_SUM, int)\n", call_num);
                    return MPI_SUCCESS;
                }
            }
        }
    }

    // Fallback to MPI
    return orig_MPI_Accumulate(origin_addr, origin_count, origin_datatype,
                                target_rank, target_disp, target_count,
                                target_datatype, op, win);
}

// ============================================================================
// MPI Window Synchronization with CXL Fences
// ============================================================================

int MPI_Win_fence(int assert, MPI_Win win) {
    static _Atomic int fence_count = 0;
    int call_num = atomic_fetch_add(&fence_count, 1);

    LOG_DEBUG("MPI_Win_fence[%d]: assert=%d\n", call_num, assert);

    LOAD_ORIGINAL(MPI_Win_fence);

    cxl_window_t *cxl_win = find_cxl_window(win);

    if (cxl_win && cxl_win->cxl_enabled && cxl_win->shm) {
        // Full memory barrier to ensure all CXL writes are visible
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        // Track fence epochs for debugging
        cxl_win_rank_info_t *my_info = &cxl_win->shm->ranks[cxl_win->my_rank];
        uint64_t my_fence = atomic_fetch_add(&my_info->fence_counter, 1) + 1;
        atomic_fetch_add(&cxl_win->shm->global_fence, 1);

        // Use MPI fence for synchronization - it will barrier internally
        int ret = orig_MPI_Win_fence(assert, win);

        // Another memory barrier after synchronization
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        LOG_DEBUG("MPI_Win_fence[%d]: CXL fence completed (epoch=%lu)\n", call_num, my_fence);
        return ret;
    }

    return orig_MPI_Win_fence(assert, win);
}

int MPI_Win_lock(int lock_type, int rank, int assert, MPI_Win win) {
    LOG_DEBUG("MPI_Win_lock: type=%d, rank=%d\n", lock_type, rank);

    LOAD_ORIGINAL(MPI_Win_lock);

    cxl_window_t *cxl_win = find_cxl_window(win);

    if (cxl_win && cxl_win->cxl_enabled && cxl_win->shm) {
        cxl_win_rank_info_t *target_info = &cxl_win->shm->ranks[rank];

        if (lock_type == MPI_LOCK_EXCLUSIVE) {
            // Spin until we can get exclusive lock
            uint32_t expected = (uint32_t)-1;
            while (!atomic_compare_exchange_weak(&target_info->exclusive_lock,
                                                  &expected, cxl_win->my_rank)) {
                expected = (uint32_t)-1;
                __builtin_ia32_pause();
            }
            atomic_store(&target_info->sync_state, CXL_WIN_LOCK_EXCLUSIVE);
        } else {
            // Shared lock - wait for no exclusive lock, then increment count
            while (atomic_load(&target_info->exclusive_lock) != (uint32_t)-1) {
                __builtin_ia32_pause();
            }
            atomic_fetch_add(&target_info->lock_count, 1);
            atomic_store(&target_info->sync_state, CXL_WIN_LOCK_SHARED);
        }

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        LOG_DEBUG("MPI_Win_lock: CXL lock acquired (type=%d, rank=%d)\n", lock_type, rank);
    }

    return orig_MPI_Win_lock(lock_type, rank, assert, win);
}

int MPI_Win_unlock(int rank, MPI_Win win) {
    LOG_DEBUG("MPI_Win_unlock: rank=%d\n", rank);

    LOAD_ORIGINAL(MPI_Win_unlock);

    cxl_window_t *cxl_win = find_cxl_window(win);

    if (cxl_win && cxl_win->cxl_enabled && cxl_win->shm) {
        cxl_win_rank_info_t *target_info = &cxl_win->shm->ranks[rank];

        __atomic_thread_fence(__ATOMIC_RELEASE);

        uint32_t state = atomic_load(&target_info->sync_state);
        if (state == CXL_WIN_LOCK_EXCLUSIVE) {
            atomic_store(&target_info->exclusive_lock, (uint32_t)-1);
        } else if (state == CXL_WIN_LOCK_SHARED) {
            atomic_fetch_sub(&target_info->lock_count, 1);
        }
        atomic_store(&target_info->sync_state, CXL_WIN_UNLOCKED);

        LOG_DEBUG("MPI_Win_unlock: CXL lock released (rank=%d)\n", rank);
    }

    return orig_MPI_Win_unlock(rank, win);
}

int MPI_Win_flush(int rank, MPI_Win win) {
    LOG_DEBUG("MPI_Win_flush: rank=%d\n", rank);

    LOAD_ORIGINAL(MPI_Win_flush);

    // Memory fence for CXL
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    return orig_MPI_Win_flush(rank, win);
}

// ============================================================================
// MPI Collective Operations with CXL Shared Memory
// ============================================================================

int MPI_Barrier(MPI_Comm comm) {
    static _Atomic int barrier_count = 0;
    int call_num = atomic_fetch_add(&barrier_count, 1);

    LOG_DEBUG("MPI_Barrier[%d]\n", call_num);

    LOAD_ORIGINAL(MPI_Barrier);

    // Note: CXL barrier disabled due to cache coherency issues across nodes
    // Always use original MPI_Barrier for reliable synchronization

    return orig_MPI_Barrier(comm);
}

int MPI_Bcast(void *buffer, int count, MPI_Datatype datatype, int root, MPI_Comm comm) {
    static _Atomic int bcast_count = 0;
    int call_num = atomic_fetch_add(&bcast_count, 1);

    int type_size;
    MPI_Type_size(datatype, &type_size);
    size_t total_size = (size_t)count * type_size;

    LOG_DEBUG("MPI_Bcast[%d]: count=%d, root=%d, size=%zu\n", call_num, count, root, total_size);

    LOAD_ORIGINAL(MPI_Bcast);

    // Try CXL broadcast for COMM_WORLD
    if (g_cxl.cxl_comm_enabled && comm == MPI_COMM_WORLD && total_size <= 4096 &&
        g_cxl.world_size <= 64) {

        LOAD_ORIGINAL(MPI_Barrier);

        // Root allocates buffer and writes data
        void *root_buf = NULL;
        if (g_cxl.my_rank == root) {
            root_buf = allocate_cxl_memory(total_size);
            if (root_buf) {
                cxl_safe_memcpy(root_buf, buffer, total_size);
                cxl_flush_range(root_buf, total_size);
            }
        }

        // Root registers its buffer (or NULL for non-root)
        cxl_collective_register_buffer(g_cxl.my_rank, root_buf);

        // Use MPI_Barrier for reliable synchronization across nodes
        orig_MPI_Barrier(comm);

        // All non-root ranks get root's buffer address and read
        if (g_cxl.my_rank != root) {
            void *bcast_buf = cxl_collective_get_buffer(root);
            if (!bcast_buf) {
                LOG_WARN("MPI_Bcast[%d]: Root buffer unavailable, fallback\n", call_num);
                goto bcast_fallback;
            }
            cxl_safe_memcpy(buffer, bcast_buf, total_size);
        }

        // Final barrier before returning
        orig_MPI_Barrier(comm);

        LOG_DEBUG("MPI_Bcast[%d]: CXL optimized (%zu bytes from root %d)\n",
                  call_num, total_size, root);
        return MPI_SUCCESS;
    }

bcast_fallback:
    return orig_MPI_Bcast(buffer, count, datatype, root, comm);
}

int MPI_Reduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype,
               MPI_Op op, int root, MPI_Comm comm) {
    static _Atomic int reduce_count = 0;
    int call_num = atomic_fetch_add(&reduce_count, 1);

    LOG_DEBUG("MPI_Reduce[%d]: count=%d, root=%d\n", call_num, count, root);

    LOAD_ORIGINAL(MPI_Reduce);
    return orig_MPI_Reduce(sendbuf, recvbuf, count, datatype, op, root, comm);
}

int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count, MPI_Datatype datatype,
                  MPI_Op op, MPI_Comm comm) {
    static _Atomic int allreduce_count = 0;
    int call_num = atomic_fetch_add(&allreduce_count, 1);

    int type_size;
    MPI_Type_size(datatype, &type_size);
    size_t total_size = (size_t)count * type_size;

    LOG_DEBUG("MPI_Allreduce[%d]: count=%d, size=%zu\n", call_num, count, total_size);

    LOAD_ORIGINAL(MPI_Allreduce);

    // For small allreduce on COMM_WORLD with SUM, try CXL optimization
    // Note: Skip if sendbuf is MPI_IN_PLACE to avoid complexity
    if (g_cxl.cxl_comm_enabled && comm == MPI_COMM_WORLD &&
        sendbuf != MPI_IN_PLACE &&
        op == MPI_SUM && total_size <= 4096 && g_cxl.world_size <= 64 &&
        (datatype == MPI_DOUBLE || datatype == MPI_FLOAT || datatype == MPI_INT ||
         datatype == MPI_LONG || datatype == MPI_LONG_LONG)) {

        LOAD_ORIGINAL(MPI_Barrier);

        // Allocate per-rank buffer in shared memory
        void *my_buf = allocate_cxl_memory(total_size);
        if (my_buf) {
            // Copy my data to shared buffer (use safe copy for CXL)
            cxl_safe_memcpy(my_buf, sendbuf, total_size);
            cxl_flush_range(my_buf, total_size);

            // Register my buffer location so other ranks can find it
            cxl_collective_register_buffer(g_cxl.my_rank, my_buf);

            // Use MPI_Barrier for reliable synchronization across nodes
            orig_MPI_Barrier(comm);

            // Initialize result with my own data
            memcpy(recvbuf, sendbuf, total_size);

            // Read and reduce data from all other ranks
            for (int r = 0; r < g_cxl.world_size; r++) {
                if (r != g_cxl.my_rank) {
                    void *their_buf = cxl_collective_get_buffer(r);
                    if (their_buf) {
                        // Perform reduction based on datatype
                        if (datatype == MPI_DOUBLE) {
                            double *dst = (double *)recvbuf;
                            double *src = (double *)their_buf;
                            for (int i = 0; i < count; i++) {
                                dst[i] += src[i];
                            }
                        } else if (datatype == MPI_FLOAT) {
                            float *dst = (float *)recvbuf;
                            float *src = (float *)their_buf;
                            for (int i = 0; i < count; i++) {
                                dst[i] += src[i];
                            }
                        } else if (datatype == MPI_INT) {
                            int *dst = (int *)recvbuf;
                            int *src = (int *)their_buf;
                            for (int i = 0; i < count; i++) {
                                dst[i] += src[i];
                            }
                        } else if (datatype == MPI_LONG) {
                            long *dst = (long *)recvbuf;
                            long *src = (long *)their_buf;
                            for (int i = 0; i < count; i++) {
                                dst[i] += src[i];
                            }
                        } else if (datatype == MPI_LONG_LONG) {
                            long long *dst = (long long *)recvbuf;
                            long long *src = (long long *)their_buf;
                            for (int i = 0; i < count; i++) {
                                dst[i] += src[i];
                            }
                        }
                    } else {
                        // Rank's buffer not available - fall back to original
                        LOG_WARN("MPI_Allreduce[%d]: Rank %d buffer unavailable, fallback\n",
                                 call_num, r);
                        goto fallback;
                    }
                }
            }

            // Final barrier before returning (ensure all ranks finished reading)
            orig_MPI_Barrier(comm);

            LOG_DEBUG("MPI_Allreduce[%d]: CXL optimized SUM (%zu bytes)\n", call_num, total_size);
            return MPI_SUCCESS;
        }
    }

fallback:
    return orig_MPI_Allreduce(sendbuf, recvbuf, count, datatype, op, comm);
}

int MPI_Allgather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                  void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm) {
    static _Atomic int allgather_count = 0;
    int call_num = atomic_fetch_add(&allgather_count, 1);

    int send_size;
    MPI_Type_size(sendtype, &send_size);
    size_t send_bytes = (size_t)sendcount * send_size;

    LOG_DEBUG("MPI_Allgather[%d]: sendcount=%d, recvcount=%d\n", call_num, sendcount, recvcount);

    LOAD_ORIGINAL(MPI_Allgather);

    // CXL-optimized allgather for COMM_WORLD
    // Skip MPI_IN_PLACE to avoid complexity
    if (g_cxl.cxl_comm_enabled && comm == MPI_COMM_WORLD &&
        sendbuf != MPI_IN_PLACE &&
        send_bytes <= 4096 && g_cxl.world_size <= 64) {

        LOAD_ORIGINAL(MPI_Barrier);

        // Each rank allocates its own contribution buffer
        void *my_buf = allocate_cxl_memory(send_bytes);
        if (my_buf) {
            // Copy my contribution to shared memory (use safe copy for CXL)
            cxl_safe_memcpy(my_buf, sendbuf, send_bytes);
            cxl_flush_range(my_buf, send_bytes);

            // Register my buffer location
            cxl_collective_register_buffer(g_cxl.my_rank, my_buf);

            // Use MPI_Barrier for reliable synchronization across nodes
            orig_MPI_Barrier(comm);

            // Each rank reads all contributions in order
            for (int r = 0; r < g_cxl.world_size; r++) {
                void *their_buf = cxl_collective_get_buffer(r);
                if (!their_buf) {
                    LOG_WARN("MPI_Allgather[%d]: Rank %d buffer unavailable, fallback\n",
                             call_num, r);
                    goto allgather_fallback;
                }
                void *dst = (char *)recvbuf + r * send_bytes;
                cxl_safe_memcpy(dst, their_buf, send_bytes);
            }

            // Final barrier before returning
            orig_MPI_Barrier(comm);

            LOG_DEBUG("MPI_Allgather[%d]: CXL optimized (%zu bytes each)\n", call_num, send_bytes);
            return MPI_SUCCESS;
        }
    }

allgather_fallback:
    return orig_MPI_Allgather(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm);
}

int MPI_Alltoall(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                 void *recvbuf, int recvcount, MPI_Datatype recvtype, MPI_Comm comm) {
    static _Atomic int alltoall_count = 0;
    static _Atomic int cxl_alltoall_count = 0;
    int call_num = atomic_fetch_add(&alltoall_count, 1);

    int send_size, recv_size;
    MPI_Type_size(sendtype, &send_size);
    MPI_Type_size(recvtype, &recv_size);
    size_t send_bytes = (size_t)sendcount * send_size;
    size_t recv_bytes = (size_t)recvcount * recv_size;

    LOG_DEBUG("MPI_Alltoall[%d]: sendcount=%d, recvcount=%d\n", call_num, sendcount, recvcount);

    LOAD_ORIGINAL(MPI_Alltoall);

    // CXL-optimized all-to-all for COMM_WORLD
    if (g_cxl.cxl_comm_enabled && comm == MPI_COMM_WORLD && send_bytes <= 4096 &&
        g_cxl.world_size <= 64) {
        int n = g_cxl.world_size;
        size_t row_size = send_bytes * n;  // Total data this rank sends

        LOAD_ORIGINAL(MPI_Barrier);

        // Each rank allocates buffer for its outgoing row
        void *my_row = allocate_cxl_memory(row_size);

        if (my_row) {
            // Copy my send data to shared memory (use safe copy for CXL)
            cxl_safe_memcpy(my_row, sendbuf, row_size);
            cxl_flush_range(my_row, row_size);

            // Register my row buffer
            cxl_collective_register_buffer(g_cxl.my_rank, my_row);

            // Use MPI_Barrier for reliable synchronization across nodes
            orig_MPI_Barrier(comm);

            // Read my column - element [r][my_rank] from each rank r
            for (int r = 0; r < n; r++) {
                void *their_row = cxl_collective_get_buffer(r);
                if (!their_row) {
                    LOG_WARN("MPI_Alltoall[%d]: Rank %d buffer unavailable, fallback\n",
                             call_num, r);
                    goto alltoall_fallback;
                }
                // Element [r][my_rank] = data rank r sends to me
                void *src = (char *)their_row + g_cxl.my_rank * send_bytes;
                void *dst = (char *)recvbuf + r * recv_bytes;
                cxl_safe_memcpy(dst, src, recv_bytes);
            }

            // Final barrier before returning
            orig_MPI_Barrier(comm);

            int cxl_num = atomic_fetch_add(&cxl_alltoall_count, 1);
            LOG_DEBUG("MPI_Alltoall[%d]: CXL direct #%d (%zu bytes per rank)\n",
                      call_num, cxl_num, send_bytes);
            return MPI_SUCCESS;
        }
    }

alltoall_fallback:
    return orig_MPI_Alltoall(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, comm);
}

int MPI_Gather(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
               void *recvbuf, int recvcount, MPI_Datatype recvtype,
               int root, MPI_Comm comm) {
    static _Atomic int gather_count = 0;
    int call_num = atomic_fetch_add(&gather_count, 1);

    LOG_DEBUG("MPI_Gather[%d]: sendcount=%d, root=%d\n", call_num, sendcount, root);

    LOAD_ORIGINAL(MPI_Gather);

    // CXL-optimized gather
    if (g_cxl.cxl_comm_enabled && comm == MPI_COMM_WORLD && g_cxl.world_size <= 64) {
        int send_size;
        MPI_Type_size(sendtype, &send_size);
        size_t send_bytes = (size_t)sendcount * send_size;

        if (send_bytes <= 4096) {
            LOAD_ORIGINAL(MPI_Barrier);

            // Each rank allocates its own contribution buffer
            void *my_buf = allocate_cxl_memory(send_bytes);

            if (my_buf) {
                // Copy my contribution to shared memory (use safe copy for CXL)
                cxl_safe_memcpy(my_buf, sendbuf, send_bytes);
                cxl_flush_range(my_buf, send_bytes);

                // Register my buffer location
                cxl_collective_register_buffer(g_cxl.my_rank, my_buf);

                // Use MPI_Barrier for reliable synchronization across nodes
                orig_MPI_Barrier(comm);

                // Root reads all contributions
                if (g_cxl.my_rank == root) {
                    for (int r = 0; r < g_cxl.world_size; r++) {
                        void *their_buf = cxl_collective_get_buffer(r);
                        if (!their_buf) {
                            LOG_WARN("MPI_Gather[%d]: Rank %d buffer unavailable, fallback\n",
                                     call_num, r);
                            goto gather_fallback;
                        }
                        void *dst = (char *)recvbuf + r * send_bytes;
                        cxl_safe_memcpy(dst, their_buf, send_bytes);
                    }
                }

                // Final barrier before returning
                orig_MPI_Barrier(comm);

                LOG_DEBUG("MPI_Gather[%d]: CXL optimized\n", call_num);
                return MPI_SUCCESS;
            }
        }
    }

gather_fallback:
    return orig_MPI_Gather(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, root, comm);
}

int MPI_Scatter(const void *sendbuf, int sendcount, MPI_Datatype sendtype,
                void *recvbuf, int recvcount, MPI_Datatype recvtype,
                int root, MPI_Comm comm) {
    static _Atomic int scatter_count = 0;
    int call_num = atomic_fetch_add(&scatter_count, 1);

    LOG_DEBUG("MPI_Scatter[%d]: sendcount=%d, root=%d\n", call_num, sendcount, root);

    LOAD_ORIGINAL(MPI_Scatter);

    // CXL-optimized scatter
    if (g_cxl.cxl_comm_enabled && comm == MPI_COMM_WORLD && g_cxl.world_size <= 64) {
        int send_size;
        MPI_Type_size(sendtype, &send_size);
        size_t send_bytes = (size_t)sendcount * send_size;

        if (send_bytes <= 4096) {
            size_t total_size = send_bytes * g_cxl.world_size;

            LOAD_ORIGINAL(MPI_Barrier);

            // Root allocates buffer and writes all data
            void *root_buf = NULL;
            if (g_cxl.my_rank == root) {
                root_buf = allocate_cxl_memory(total_size);
                if (root_buf) {
                    cxl_safe_memcpy(root_buf, sendbuf, total_size);
                    cxl_flush_range(root_buf, total_size);
                }
            }

            // Root registers its buffer (or NULL for non-root)
            cxl_collective_register_buffer(g_cxl.my_rank, root_buf);

            // Use MPI_Barrier for reliable synchronization across nodes
            orig_MPI_Barrier(comm);

            // All ranks get root's buffer address and read their portion
            void *scatter_buf = cxl_collective_get_buffer(root);
            if (!scatter_buf) {
                LOG_WARN("MPI_Scatter[%d]: Root buffer unavailable, fallback\n", call_num);
                goto scatter_fallback;
            }

            void *my_slot = (char *)scatter_buf + g_cxl.my_rank * send_bytes;
            cxl_safe_memcpy(recvbuf, my_slot, send_bytes);

            // Final barrier before returning
            orig_MPI_Barrier(comm);

            LOG_DEBUG("MPI_Scatter[%d]: CXL optimized\n", call_num);
            return MPI_SUCCESS;
        }
    }

scatter_fallback:
    return orig_MPI_Scatter(sendbuf, sendcount, sendtype, recvbuf, recvcount, recvtype, root, comm);
}

// Override problematic OpenMPI internal function to prevent SIGILL
int ompi_errhandler_init(void) {
    static int initialized = 0;
    static typeof(ompi_errhandler_init) *orig_ompi_errhandler_init = NULL;

    if (initialized) {
        LOG_TRACE("ompi_errhandler_init already initialized, skipping\n");
        return 0;  // Return success without doing anything
    }

    // Try to load the original function
    if (!orig_ompi_errhandler_init) {
        orig_ompi_errhandler_init = dlsym(RTLD_NEXT, "ompi_errhandler_init");
        if (!orig_ompi_errhandler_init) {
            // If we can't find it, that's okay - just mark as initialized
            LOG_DEBUG("ompi_errhandler_init not found in RTLD_NEXT, returning success\n");
            initialized = 1;
            return 0;  // Return MPI_SUCCESS (0)
        }
    }

    LOG_TRACE("Calling original ompi_errhandler_init at %p\n", orig_ompi_errhandler_init);

    // Set flag first to prevent recursion
    initialized = 1;

    // Call original if it exists
    if (orig_ompi_errhandler_init) {
        return orig_ompi_errhandler_init();
    }

    return 0;  // Return success
}

// Alternative: completely stub out the function if it's causing issues
// Uncomment this version if the above still causes SIGILL
/*
int ompi_errhandler_init(void) {
    static int called = 0;
    if (!called) {
        called = 1;
        LOG_DEBUG("ompi_errhandler_init stubbed out to prevent SIGILL\n");
    }
    return 0;  // Always return success
}
*/

// Library constructor
__attribute__((constructor))
static void shim_init(void) {
    // Set up signal handlers for debugging
    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);

    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    fprintf(stderr, "\n");
    fprintf(stderr, "┌──────────────────────────────────────────────────────────┐\n");
    fprintf(stderr, "│    CXL MPI SHIM LIBRARY v%s - Remotable Pointer Edition  │\n", SHIM_VERSION);
    fprintf(stderr, "├──────────────────────────────────────────────────────────┤\n");
    fprintf(stderr, "│ Host: %-52s │\n", hostname);
    fprintf(stderr, "│ PID:  %-52d │\n", getpid());
    fprintf(stderr, "│ Time: %-52ld │\n", time(NULL));
    fprintf(stderr, "├──────────────────────────────────────────────────────────┤\n");
    fprintf(stderr, "│ FEATURES:                                                │\n");
    fprintf(stderr, "│   - CXL Direct Send/Recv via shared DAX device          │\n");
    fprintf(stderr, "│   - Remotable pointers (offset-based addressing)        │\n");
    fprintf(stderr, "│   - Per-rank message queues in shared memory            │\n");
    fprintf(stderr, "│   - Inline small messages (<4KB) optimization           │\n");
    fprintf(stderr, "├──────────────────────────────────────────────────────────┤\n");
    fprintf(stderr, "│ CONFIGURATION:                                           │\n");
    fprintf(stderr, "│   CXL_DAX_PATH: %-40s │\n",
            getenv("CXL_DAX_PATH") ? getenv("CXL_DAX_PATH") : "(not set - using shm)");
    fprintf(stderr, "│   CXL_DIRECT: %-42s │\n",
            getenv("CXL_DIRECT") ? "ENABLED" : "(auto for DAX)");
    fprintf(stderr, "│   CXL_SHIM_VERBOSE: %-36s │\n",
            getenv("CXL_SHIM_VERBOSE") ? "YES" : "NO");
    fprintf(stderr, "│   CXL_SHIM_TRACE: %-38s │\n",
            getenv("CXL_SHIM_TRACE") ? "YES" : "NO");
    fprintf(stderr, "│   CXL_DAX_RESET: %-39s │\n",
            getenv("CXL_DAX_RESET") ? "YES" : "NO");
    fprintf(stderr, "└──────────────────────────────────────────────────────────┘\n");
    fprintf(stderr, "\n");
    fflush(stderr);

    // Pre-load some functions
    void *handle = dlopen(NULL, RTLD_LAZY);
    if (handle) {
        void *mpi_init = dlsym(handle, "MPI_Init");
        void *pmpi_init = dlsym(handle, "PMPI_Init");
        LOG_DEBUG("Found MPI_Init at %p, PMPI_Init at %p\n", mpi_init, pmpi_init);
        dlclose(handle);
    }
}

// Library destructor
__attribute__((destructor))
static void shim_cleanup(void) {
    LOG_INFO("CXL MPI Shim unloading (total hooks: %d)\n", g_hook_count);
    
    if (g_cxl.initialized) {
        LOG_INFO("Final CXL memory usage: %zu/%zu bytes (%.1f%%)\n",
                 g_cxl.used, g_cxl.size, 100.0 * g_cxl.used / g_cxl.size);
    }
}