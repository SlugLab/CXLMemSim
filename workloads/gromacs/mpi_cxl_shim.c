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
#define SHIM_VERSION "2.0"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

// Add color output for better visibility
#define RED     "\x1b[31m"
#define GREEN   "\x1b[32m"
#define YELLOW  "\x1b[33m"
#define BLUE    "\x1b[34m"
#define MAGENTA "\x1b[35m"
#define CYAN    "\x1b[36m"
#define RESET   "\x1b[0m"

typedef struct {
    void *base;
    size_t size;
    size_t used;
    int fd;
    bool initialized;
    pthread_mutex_t lock;
    char *type;  // "dax" or "shm"
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
    .initialized = false
};

static mem_mapping_t *g_mappings = NULL;
static pthread_mutex_t g_mappings_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_hook_count = 0;
static _Atomic bool g_in_mpi_call = false;

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

        g_cxl.base = mmap(NULL, cxl_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_cxl.fd, 0);
        if (g_cxl.base == MAP_FAILED) {
            LOG_ERROR("Failed to map DAX device %s: %s\n", dax_path, strerror(errno));
            close(g_cxl.fd);
            goto use_shm;
        }

        g_cxl.type = "dax";
        LOG_INFO("Mapped DAX device %s: %zu bytes (%zu MB) at %p\n",
                 dax_path, cxl_size, cxl_size / (1024*1024), g_cxl.base);

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

        g_cxl.base = mmap(NULL, cxl_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_cxl.fd, 0);
        if (g_cxl.base == MAP_FAILED) {
            LOG_ERROR("Failed to map shared memory: %s\n", strerror(errno));
            close(g_cxl.fd);
            pthread_mutex_unlock(&g_cxl.lock);
            return;
        }

        g_cxl.type = "shm";
        LOG_INFO("Mapped shared memory %s: %zu bytes (%zu MB) at %p\n",
                 shm_name, cxl_size, cxl_size / (1024*1024), g_cxl.base);

        // For shared memory, also use first cacheline for coordination
        g_cxl.used = CACHELINE_SIZE;
    }

    g_cxl.size = cxl_size;
    g_cxl.initialized = true;

    LOG_INFO("CXL memory initialized: type=%s, size=%zu MB, base=%p\n",
             g_cxl.type, cxl_size / (1024*1024), g_cxl.base);

    pthread_mutex_unlock(&g_cxl.lock);
}

// Allocate from CXL memory pool
static void *allocate_cxl_memory(size_t size) {
    if (!g_cxl.initialized) {
        init_cxl_memory();
        if (!g_cxl.initialized) return NULL;
    }

    // Align size
    size = (size + CXL_ALIGNMENT - 1) & ~(CXL_ALIGNMENT - 1);

    // For DAX/shared memory, use atomic operations on the allocation counter
    if (strcmp(g_cxl.type, "dax") == 0 || strcmp(g_cxl.type, "shm") == 0) {
        // Use first 8 bytes of the shared region as atomic allocation counter
        _Atomic size_t *alloc_counter = (_Atomic size_t *)g_cxl.base;

        size_t old_used = atomic_fetch_add(alloc_counter, size);
        size_t new_used = old_used + size;

        // Check if we have space (accounting for the counter itself)
        if (new_used > g_cxl.size) {
            // Roll back
            atomic_fetch_sub(alloc_counter, size);
            LOG_WARN("Out of CXL memory: requested=%zu, available=%zu\n",
                     size, g_cxl.size - old_used);
            return NULL;
        }

        void *ptr = (char *)g_cxl.base + old_used;

        LOG_TRACE("Allocated %zu bytes at offset %zu (total used: %zu/%zu) [atomic]\n",
                  size, old_used, new_used, g_cxl.size);

        return ptr;
    } else {
        // Local allocation (shouldn't happen but keep for safety)
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
        LOG_INFO("MPI_Init completed: rank=%d/%d, CXL=%s\n", 
                 rank, size, g_cxl.initialized ? "initialized" : "not initialized");
    } else {
        LOG_ERROR("MPI_Init failed with code %d\n", ret);
    }
    
    return ret;
}

int MPI_Finalize(void) {
    LOG_INFO("=== MPI_Finalize HOOK CALLED ===\n");

    LOAD_ORIGINAL(MPI_Finalize);

    int ret = orig_MPI_Finalize();

    // Cleanup CXL memory
    if (g_cxl.initialized) {
        LOG_INFO("Cleaning up CXL memory (used %zu/%zu bytes)\n", g_cxl.used, g_cxl.size);
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
    }

    LOG_INFO("MPI_Finalize completed (total hooks: %d)\n", g_hook_count);

    return ret;
}

int MPI_Send(const void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm) {
    static _Atomic int send_count = 0;
    int call_num = atomic_fetch_add(&send_count, 1);
    
    LOG_DEBUG("MPI_Send[%d]: count=%d, dest=%d, tag=%d, buf=%p\n", 
              call_num, count, dest, tag, buf);
    
    LOAD_ORIGINAL(MPI_Send);
    
    // Optional: copy to CXL memory
    void *send_buf = (void *)buf;
    if (g_cxl.initialized && getenv("CXL_SHIM_COPY_SEND")) {
        int size;
        MPI_Type_size(datatype, &size);
        size_t total_size = (size_t)count * size;
        
        void *cxl_buf = allocate_cxl_memory(total_size);
        if (cxl_buf) {
            memcpy(cxl_buf, buf, total_size);
            send_buf = cxl_buf;
            LOG_TRACE("MPI_Send[%d]: copied %zu bytes to CXL at %p\n", 
                      call_num, total_size, cxl_buf);
        }
    }
    
    return orig_MPI_Send(send_buf, count, datatype, dest, tag, comm);
}

int MPI_Recv(void *buf, int count, MPI_Datatype datatype, int source, int tag, 
             MPI_Comm comm, MPI_Status *status) {
    static _Atomic int recv_count = 0;
    int call_num = atomic_fetch_add(&recv_count, 1);
    
    LOG_DEBUG("MPI_Recv[%d]: count=%d, source=%d, tag=%d, buf=%p\n", 
              call_num, count, source, tag, buf);
    
    LOAD_ORIGINAL(MPI_Recv);
    
    void *recv_buf = buf;
    void *cxl_buf = NULL;
    
    if (g_cxl.initialized && getenv("CXL_SHIM_COPY_RECV")) {
        int size;
        MPI_Type_size(datatype, &size);
        size_t total_size = (size_t)count * size;
        
        cxl_buf = allocate_cxl_memory(total_size);
        if (cxl_buf) {
            recv_buf = cxl_buf;
            LOG_TRACE("MPI_Recv[%d]: using CXL buffer at %p (size=%zu)\n", 
                      call_num, cxl_buf, total_size);
        }
    }
    
    int ret = orig_MPI_Recv(recv_buf, count, datatype, source, tag, comm, status);
    
    if (cxl_buf && ret == MPI_SUCCESS) {
        int size;
        MPI_Type_size(datatype, &size);
        size_t total_size = (size_t)count * size;
        memcpy(buf, cxl_buf, total_size);
        LOG_TRACE("MPI_Recv[%d]: copied %zu bytes from CXL\n", call_num, total_size);
    }
    
    return ret;
}

// Add more MPI function hooks as needed...

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
    fprintf(stderr, "┌────────────────────────────────────────────────┐\n");
    fprintf(stderr, "│      CXL MPI SHIM LIBRARY v%s LOADED          │\n", SHIM_VERSION);
    fprintf(stderr, "├────────────────────────────────────────────────┤\n");
    fprintf(stderr, "│ Host: %-40s │\n", hostname);
    fprintf(stderr, "│ PID:  %-40d │\n", getpid());
    fprintf(stderr, "│ Time: %-40ld │\n", time(NULL));
    fprintf(stderr, "├────────────────────────────────────────────────┤\n");
    fprintf(stderr, "│ LD_PRELOAD: %-34s │\n", 
            getenv("LD_PRELOAD") ? "SET" : "NOT SET");
    fprintf(stderr, "│ CXL_SHIM_VERBOSE: %-28s │\n", 
            getenv("CXL_SHIM_VERBOSE") ? "YES" : "NO");
    fprintf(stderr, "│ CXL_DAX_PATH: %-32s │\n", 
            getenv("CXL_DAX_PATH") ? getenv("CXL_DAX_PATH") : "NOT SET");
    fprintf(stderr, "└────────────────────────────────────────────────┘\n");
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