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

#define CACHELINE_SIZE 64
#define DEFAULT_CXL_SIZE (4UL * 1024 * 1024 * 1024)  // 4GB default
#define CXL_ALIGNMENT 4096

typedef struct {
    void *base;
    size_t size;
    int fd;
    bool initialized;
    pthread_mutex_t lock;
} cxl_mem_t;

typedef struct {
    void *cxl_addr;
    void *orig_addr;
    size_t size;
    int ref_count;
} mem_mapping_t;

#define MAX_MAPPINGS 65536
static cxl_mem_t g_cxl = {0};
static mem_mapping_t g_mappings[MAX_MAPPINGS] = {0};
static _Atomic size_t g_next_offset = 0;
static pthread_mutex_t g_mappings_lock = PTHREAD_MUTEX_INITIALIZER;

// Function pointers for original MPI functions
static int (*orig_MPI_Init)(int *, char ***) = NULL;
static int (*orig_MPI_Finalize)(void) = NULL;
static int (*orig_MPI_Send)(const void *, int, MPI_Datatype, int, int, MPI_Comm) = NULL;
static int (*orig_MPI_Recv)(void *, int, MPI_Datatype, int, int, MPI_Comm, MPI_Status *) = NULL;
static int (*orig_MPI_Isend)(const void *, int, MPI_Datatype, int, int, MPI_Comm, MPI_Request *) = NULL;
static int (*orig_MPI_Irecv)(void *, int, MPI_Datatype, int, int, MPI_Comm, MPI_Request *) = NULL;
static int (*orig_MPI_Alloc_mem)(MPI_Aint, MPI_Info, void *) = NULL;
static int (*orig_MPI_Free_mem)(void *) = NULL;
static int (*orig_MPI_Win_allocate)(MPI_Aint, int, MPI_Info, MPI_Comm, void *, MPI_Win *) = NULL;
static int (*orig_MPI_Win_allocate_shared)(MPI_Aint, int, MPI_Info, MPI_Comm, void *, MPI_Win *) = NULL;

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
    
    if (!dax_path) {
        // Use shared memory fallback
        const char *shm_name = "/cxlmemsim_mpi_shared";
        g_cxl.fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
        if (g_cxl.fd < 0) {
            fprintf(stderr, "[CXL_SHIM] Failed to open shared memory: %s\n", strerror(errno));
            goto fail;
        }
        
        if (ftruncate(g_cxl.fd, cxl_size) != 0) {
            fprintf(stderr, "[CXL_SHIM] Failed to resize shared memory: %s\n", strerror(errno));
            close(g_cxl.fd);
            goto fail;
        }
        
        g_cxl.base = mmap(NULL, cxl_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_cxl.fd, 0);
    } else {
        // Use DAX device
        g_cxl.fd = open(dax_path, O_RDWR | O_CLOEXEC);
        if (g_cxl.fd < 0) {
            fprintf(stderr, "[CXL_SHIM] Failed to open DAX device %s: %s\n", dax_path, strerror(errno));
            goto fail;
        }
        
        // Try to read actual DAX size from sysfs
        char sysfs_path[512];
        const char *base = strrchr(dax_path, '/');
        if (base) {
            snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/dax/devices/%s/size", base + 1);
            FILE *f = fopen(sysfs_path, "r");
            if (f) {
                unsigned long long dax_size = 0;
                if (fscanf(f, "%llu", &dax_size) == 1 && dax_size > 0) {
                    cxl_size = (size_t)dax_size;
                }
                fclose(f);
            }
        }
        
        g_cxl.base = mmap(NULL, cxl_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_cxl.fd, 0);
    }
    
    if (g_cxl.base == MAP_FAILED) {
        fprintf(stderr, "[CXL_SHIM] Failed to map CXL memory: %s\n", strerror(errno));
        close(g_cxl.fd);
        goto fail;
    }
    
    g_cxl.size = cxl_size;
    g_cxl.initialized = true;
    atomic_store(&g_next_offset, 0);
    
    if (getenv("CXL_SHIM_VERBOSE")) {
        fprintf(stderr, "[CXL_SHIM] Initialized CXL memory: base=%p size=%zu\n", g_cxl.base, g_cxl.size);
    }
    
fail:
    pthread_mutex_unlock(&g_cxl.lock);
}

static void *allocate_cxl_memory(size_t size) {
    if (!g_cxl.initialized) {
        init_cxl_memory();
        if (!g_cxl.initialized) return NULL;
    }
    
    // Align size to page boundary
    size = (size + CXL_ALIGNMENT - 1) & ~(CXL_ALIGNMENT - 1);
    
    size_t offset = atomic_fetch_add(&g_next_offset, size);
    if (offset + size > g_cxl.size) {
        atomic_fetch_sub(&g_next_offset, size);
        fprintf(stderr, "[CXL_SHIM] Out of CXL memory\n");
        return NULL;
    }
    
    return (char *)g_cxl.base + offset;
}

static int register_mapping(void *cxl_addr, void *orig_addr, size_t size) {
    pthread_mutex_lock(&g_mappings_lock);
    
    for (int i = 0; i < MAX_MAPPINGS; i++) {
        if (g_mappings[i].cxl_addr == NULL) {
            g_mappings[i].cxl_addr = cxl_addr;
            g_mappings[i].orig_addr = orig_addr;
            g_mappings[i].size = size;
            g_mappings[i].ref_count = 1;
            pthread_mutex_unlock(&g_mappings_lock);
            return i;
        }
    }
    
    pthread_mutex_unlock(&g_mappings_lock);
    return -1;
}

static void *find_cxl_mapping(const void *orig_addr) {
    pthread_mutex_lock(&g_mappings_lock);
    
    for (int i = 0; i < MAX_MAPPINGS; i++) {
        if (g_mappings[i].orig_addr == orig_addr) {
            void *cxl_addr = g_mappings[i].cxl_addr;
            pthread_mutex_unlock(&g_mappings_lock);
            return cxl_addr;
        }
    }
    
    pthread_mutex_unlock(&g_mappings_lock);
    return NULL;
}

static void unregister_mapping(void *addr) {
    pthread_mutex_lock(&g_mappings_lock);
    
    for (int i = 0; i < MAX_MAPPINGS; i++) {
        if (g_mappings[i].cxl_addr == addr || g_mappings[i].orig_addr == addr) {
            if (--g_mappings[i].ref_count <= 0) {
                g_mappings[i].cxl_addr = NULL;
                g_mappings[i].orig_addr = NULL;
                g_mappings[i].size = 0;
            }
            break;
        }
    }
    
    pthread_mutex_unlock(&g_mappings_lock);
}

// MPI Function Interceptions
int MPI_Init(int *argc, char ***argv) {
    if (!orig_MPI_Init) {
        orig_MPI_Init = dlsym(RTLD_NEXT, "MPI_Init");
    }
    
    init_cxl_memory();
    
    int ret = orig_MPI_Init(argc, argv);
    
    if (getenv("CXL_SHIM_VERBOSE")) {
        int rank;
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        fprintf(stderr, "[CXL_SHIM] MPI_Init completed on rank %d\n", rank);
    }
    
    return ret;
}

int MPI_Finalize(void) {
    if (!orig_MPI_Finalize) {
        orig_MPI_Finalize = dlsym(RTLD_NEXT, "MPI_Finalize");
    }
    
    int ret = orig_MPI_Finalize();
    
    if (g_cxl.initialized) {
        munmap(g_cxl.base, g_cxl.size);
        close(g_cxl.fd);
        g_cxl.initialized = false;
    }
    
    return ret;
}

int MPI_Send(const void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm) {
    if (!orig_MPI_Send) {
        orig_MPI_Send = dlsym(RTLD_NEXT, "MPI_Send");
    }
    
    // Check if buffer is in CXL memory or needs to be copied
    void *cxl_buf = find_cxl_mapping(buf);
    if (!cxl_buf && g_cxl.initialized && getenv("CXL_SHIM_COPY_SEND")) {
        int size;
        MPI_Type_size(datatype, &size);
        size_t total_size = (size_t)count * size;
        
        cxl_buf = allocate_cxl_memory(total_size);
        if (cxl_buf) {
            memcpy(cxl_buf, buf, total_size);
            buf = cxl_buf;
        }
    }
    
    return orig_MPI_Send(buf, count, datatype, dest, tag, comm);
}

int MPI_Recv(void *buf, int count, MPI_Datatype datatype, int source, int tag, MPI_Comm comm, MPI_Status *status) {
    if (!orig_MPI_Recv) {
        orig_MPI_Recv = dlsym(RTLD_NEXT, "MPI_Recv");
    }
    
    void *cxl_buf = find_cxl_mapping(buf);
    void *recv_buf = cxl_buf ? cxl_buf : buf;
    
    if (!cxl_buf && g_cxl.initialized && getenv("CXL_SHIM_COPY_RECV")) {
        int size;
        MPI_Type_size(datatype, &size);
        size_t total_size = (size_t)count * size;
        
        cxl_buf = allocate_cxl_memory(total_size);
        if (cxl_buf) {
            recv_buf = cxl_buf;
        }
    }
    
    int ret = orig_MPI_Recv(recv_buf, count, datatype, source, tag, comm, status);
    
    if (cxl_buf && recv_buf != buf) {
        int size;
        MPI_Type_size(datatype, &size);
        size_t total_size = (size_t)count * size;
        memcpy(buf, recv_buf, total_size);
    }
    
    return ret;
}

int MPI_Isend(const void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm, MPI_Request *request) {
    if (!orig_MPI_Isend) {
        orig_MPI_Isend = dlsym(RTLD_NEXT, "MPI_Isend");
    }
    
    void *cxl_buf = find_cxl_mapping(buf);
    if (!cxl_buf && g_cxl.initialized && getenv("CXL_SHIM_COPY_SEND")) {
        int size;
        MPI_Type_size(datatype, &size);
        size_t total_size = (size_t)count * size;
        
        cxl_buf = allocate_cxl_memory(total_size);
        if (cxl_buf) {
            memcpy(cxl_buf, buf, total_size);
            buf = cxl_buf;
        }
    }
    
    return orig_MPI_Isend(buf, count, datatype, dest, tag, comm, request);
}

int MPI_Irecv(void *buf, int count, MPI_Datatype datatype, int source, int tag, MPI_Comm comm, MPI_Request *request) {
    if (!orig_MPI_Irecv) {
        orig_MPI_Irecv = dlsym(RTLD_NEXT, "MPI_Irecv");
    }
    
    void *cxl_buf = find_cxl_mapping(buf);
    void *recv_buf = cxl_buf ? cxl_buf : buf;
    
    if (!cxl_buf && g_cxl.initialized && getenv("CXL_SHIM_COPY_RECV")) {
        int size;
        MPI_Type_size(datatype, &size);
        size_t total_size = (size_t)count * size;
        
        cxl_buf = allocate_cxl_memory(total_size);
        if (cxl_buf) {
            recv_buf = cxl_buf;
            register_mapping(cxl_buf, buf, total_size);
        }
    }
    
    return orig_MPI_Irecv(recv_buf, count, datatype, source, tag, comm, request);
}

int MPI_Alloc_mem(MPI_Aint size, MPI_Info info, void *baseptr) {
    if (!g_cxl.initialized || !getenv("CXL_SHIM_ALLOC")) {
        if (!orig_MPI_Alloc_mem) {
            orig_MPI_Alloc_mem = dlsym(RTLD_NEXT, "MPI_Alloc_mem");
        }
        return orig_MPI_Alloc_mem(size, info, baseptr);
    }
    
    void *cxl_mem = allocate_cxl_memory((size_t)size);
    if (!cxl_mem) {
        return MPI_ERR_NO_MEM;
    }
    
    *(void **)baseptr = cxl_mem;
    register_mapping(cxl_mem, cxl_mem, (size_t)size);
    
    if (getenv("CXL_SHIM_VERBOSE")) {
        fprintf(stderr, "[CXL_SHIM] MPI_Alloc_mem: allocated %ld bytes at %p\n", (long)size, cxl_mem);
    }
    
    return MPI_SUCCESS;
}

int MPI_Free_mem(void *base) {
    void *cxl_addr = find_cxl_mapping(base);
    if (cxl_addr) {
        unregister_mapping(base);
        return MPI_SUCCESS;
    }
    
    if (!orig_MPI_Free_mem) {
        orig_MPI_Free_mem = dlsym(RTLD_NEXT, "MPI_Free_mem");
    }
    return orig_MPI_Free_mem(base);
}

int MPI_Win_allocate(MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm, void *baseptr, MPI_Win *win) {
    if (!g_cxl.initialized || !getenv("CXL_SHIM_WIN")) {
        if (!orig_MPI_Win_allocate) {
            orig_MPI_Win_allocate = dlsym(RTLD_NEXT, "MPI_Win_allocate");
        }
        return orig_MPI_Win_allocate(size, disp_unit, info, comm, baseptr, win);
    }
    
    void *cxl_mem = allocate_cxl_memory((size_t)size);
    if (!cxl_mem) {
        return MPI_ERR_NO_MEM;
    }
    
    *(void **)baseptr = cxl_mem;
    register_mapping(cxl_mem, cxl_mem, (size_t)size);
    
    // Create window with CXL memory
    if (!orig_MPI_Win_allocate) {
        orig_MPI_Win_allocate = dlsym(RTLD_NEXT, "MPI_Win_allocate");
    }
    
    return orig_MPI_Win_allocate(0, disp_unit, info, comm, baseptr, win);
}

int MPI_Win_allocate_shared(MPI_Aint size, int disp_unit, MPI_Info info, MPI_Comm comm, void *baseptr, MPI_Win *win) {
    if (!g_cxl.initialized || !getenv("CXL_SHIM_WIN")) {
        if (!orig_MPI_Win_allocate_shared) {
            orig_MPI_Win_allocate_shared = dlsym(RTLD_NEXT, "MPI_Win_allocate_shared");
        }
        return orig_MPI_Win_allocate_shared(size, disp_unit, info, comm, baseptr, win);
    }
    
    void *cxl_mem = allocate_cxl_memory((size_t)size);
    if (!cxl_mem) {
        return MPI_ERR_NO_MEM;
    }
    
    *(void **)baseptr = cxl_mem;
    register_mapping(cxl_mem, cxl_mem, (size_t)size);
    
    if (getenv("CXL_SHIM_VERBOSE")) {
        int rank;
        MPI_Comm_rank(comm, &rank);
        fprintf(stderr, "[CXL_SHIM] Rank %d: MPI_Win_allocate_shared %ld bytes at %p\n", rank, (long)size, cxl_mem);
    }
    
    if (!orig_MPI_Win_allocate_shared) {
        orig_MPI_Win_allocate_shared = dlsym(RTLD_NEXT, "MPI_Win_allocate_shared");
    }
    
    return orig_MPI_Win_allocate_shared(0, disp_unit, info, comm, baseptr, win);
}

// Constructor to initialize when library is loaded
__attribute__((constructor))
static void shim_init(void) {
    pthread_mutex_init(&g_cxl.lock, NULL);
    
    if (getenv("CXL_SHIM_VERBOSE")) {
        fprintf(stderr, "[CXL_SHIM] MPI CXL shim layer loaded\n");
    }
}

// Destructor to cleanup when library is unloaded
__attribute__((destructor))
static void shim_cleanup(void) {
    if (g_cxl.initialized) {
        munmap(g_cxl.base, g_cxl.size);
        if (g_cxl.fd >= 0) close(g_cxl.fd);
    }
    pthread_mutex_destroy(&g_cxl.lock);
}