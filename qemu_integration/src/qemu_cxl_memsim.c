#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include "../include/qemu_cxl_memsim.h"

static CXLMemSimContext *g_ctx = NULL;

static uint64_t get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int connect_to_cxlmemsim(CXLMemSimContext *ctx) {
    struct sockaddr_in server_addr;
    
    ctx->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->socket_fd < 0) {
        perror("socket");
        return -1;
    }
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(ctx->port);
    
    if (inet_pton(AF_INET, ctx->host, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(ctx->socket_fd);
        return -1;
    }
    
    if (connect(ctx->socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(ctx->socket_fd);
        return -1;
    }
    
    ctx->connected = true;
    return 0;
}

static int send_request(CXLMemSimContext *ctx, CXLMemSimRequest *req, CXLMemSimResponse *resp) {
    pthread_mutex_lock(&ctx->lock);
    
    if (!ctx->connected) {
        if (connect_to_cxlmemsim(ctx) < 0) {
            pthread_mutex_unlock(&ctx->lock);
            return -1;
        }
    }
    
    ssize_t sent = send(ctx->socket_fd, req, sizeof(*req), 0);
    if (sent != sizeof(*req)) {
        perror("send");
        ctx->connected = false;
        close(ctx->socket_fd);
        pthread_mutex_unlock(&ctx->lock);
        return -1;
    }
    
    ssize_t received = recv(ctx->socket_fd, resp, sizeof(*resp), MSG_WAITALL);
    if (received != sizeof(*resp)) {
        perror("recv");
        ctx->connected = false;
        close(ctx->socket_fd);
        pthread_mutex_unlock(&ctx->lock);
        return -1;
    }
    
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

static void update_hotness(CXLMemSimContext *ctx, uint64_t addr) {
    uint64_t page_idx = addr / 4096;
    if (page_idx < ctx->hotness_map_size) {
        __sync_fetch_and_add(&ctx->hotness_map[page_idx], 1);
    }
}

int cxlmemsim_init(const char *host, int port) {
    if (g_ctx != NULL) {
        fprintf(stderr, "CXLMemSim already initialized\n");
        return -1;
    }
    
    g_ctx = calloc(1, sizeof(CXLMemSimContext));
    if (!g_ctx) {
        perror("calloc");
        return -1;
    }
    
    strncpy(g_ctx->host, host, sizeof(g_ctx->host) - 1);
    g_ctx->port = port;
    g_ctx->connected = false;
    pthread_mutex_init(&g_ctx->lock, NULL);
    
    g_ctx->hotness_map_size = 1024 * 1024;
    g_ctx->hotness_map = calloc(g_ctx->hotness_map_size, sizeof(uint64_t));
    if (!g_ctx->hotness_map) {
        perror("calloc hotness_map");
        free(g_ctx);
        g_ctx = NULL;
        return -1;
    }
    
    if (connect_to_cxlmemsim(g_ctx) < 0) {
        fprintf(stderr, "Initial connection to CXLMemSim failed (will retry on first access)\n");
    }
    
    return 0;
}

void cxlmemsim_cleanup(void) {
    if (!g_ctx) return;
    
    pthread_mutex_lock(&g_ctx->lock);
    if (g_ctx->connected) {
        close(g_ctx->socket_fd);
    }
    pthread_mutex_unlock(&g_ctx->lock);
    
    pthread_mutex_destroy(&g_ctx->lock);
    free(g_ctx->hotness_map);
    free(g_ctx);
    g_ctx = NULL;
}

MemTxResult cxl_type3_read(void* d, uint64_t addr, uint64_t *data,
    unsigned size, MemTxAttrs attrs) {
    if (!g_ctx) {
        fprintf(stderr, "CXLMemSim not initialized\n");
        return -1;
    }
    fprintf(stderr, "cxl_type3_read: %lx, %lx, %u, %u\n", addr, data, size, attrs);
    size_t offset = 0;
    while (offset < size) {
        CXLMemSimRequest req = {0};
        CXLMemSimResponse resp = {0};
        
        req.op_type = CXL_READ_OP;
        req.addr = addr + offset;
        req.size = (size - offset > CACHELINE_SIZE) ? CACHELINE_SIZE : (size - offset);
        req.timestamp = get_timestamp_ns();
        
        if (send_request(g_ctx, &req, &resp) < 0) {
            return -1;
        }
        
        memcpy((uint8_t *)data + offset, resp.data, req.size);
        
        update_hotness(g_ctx, req.addr);
        __sync_fetch_and_add(&g_ctx->total_reads, 1);
        
        offset += req.size;
    }
    
    return 0;
}

MemTxResult cxl_type3_write(void *d,uint64_t  addr, uint64_t data,
    unsigned size, MemTxAttrs attrs){
    if (!g_ctx) {
        fprintf(stderr, "CXLMemSim not initialized\n");
        return -1;
    }
    fprintf(stderr, "cxl_type3_write: %lx, %lx, %u, %u\n", addr, data, size, attrs);
    
    size_t offset = 0;
    while (offset < size) {
        CXLMemSimRequest req = {0};
        CXLMemSimResponse resp = {0};
        
        req.op_type = CXL_WRITE_OP;
        req.addr = addr + offset;
        req.size = (size - offset > CACHELINE_SIZE) ? CACHELINE_SIZE : (size - offset);
        req.timestamp = get_timestamp_ns();
        memcpy(req.data, (const uint8_t *)data + offset, req.size);
        
        if (send_request(g_ctx, &req, &resp) < 0) {
            return -1;
        }
        
        update_hotness(g_ctx, req.addr);
        __sync_fetch_and_add(&g_ctx->total_writes, 1);
        
        offset += req.size;
    }
    
    return 0;
}

uint64_t cxlmemsim_get_hotness(uint64_t addr) {
    if (!g_ctx || !g_ctx->hotness_map) return 0;
    
    uint64_t page_idx = addr / 4096;
    if (page_idx < g_ctx->hotness_map_size) {
        return g_ctx->hotness_map[page_idx];
    }
    return 0;
}

void cxlmemsim_dump_hotness_stats(void) {
    if (!g_ctx) return;
    
    printf("CXLMemSim Statistics:\n");
    printf("  Total Reads: %lu\n", g_ctx->total_reads);
    printf("  Total Writes: %lu\n", g_ctx->total_writes);
    
    uint64_t hot_pages = 0;
    uint64_t total_accesses = 0;
    for (size_t i = 0; i < g_ctx->hotness_map_size; i++) {
        if (g_ctx->hotness_map[i] > 0) {
            hot_pages++;
            total_accesses += g_ctx->hotness_map[i];
        }
    }
    
    printf("  Hot Pages: %lu\n", hot_pages);
    printf("  Total Page Accesses: %lu\n", total_accesses);
    
    if (hot_pages > 0) {
        uint64_t *sorted = malloc(hot_pages * sizeof(uint64_t));
        size_t idx = 0;
        for (size_t i = 0; i < g_ctx->hotness_map_size; i++) {
            if (g_ctx->hotness_map[i] > 0) {
                sorted[idx++] = g_ctx->hotness_map[i];
            }
        }
        
        for (size_t i = 0; i < hot_pages - 1; i++) {
            for (size_t j = i + 1; j < hot_pages; j++) {
                if (sorted[i] < sorted[j]) {
                    uint64_t tmp = sorted[i];
                    sorted[i] = sorted[j];
                    sorted[j] = tmp;
                }
            }
        }
        
        size_t top_10 = (hot_pages < 10) ? hot_pages : 10;
        printf("  Top %zu hottest pages:\n", top_10);
        for (size_t i = 0; i < top_10; i++) {
            printf("    %zu: %lu accesses\n", i + 1, sorted[i]);
        }
        
        free(sorted);
    }
}

// ============================================================================
// Keyboard Hook with Back Invalidation Support
// ============================================================================

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <semaphore.h>

// Shared memory for back invalidation
#define SHM_NAME "/cxlmemsim_kbd_hook"
#define SHM_SIZE (1024 * 1024)  // 1MB shared memory
#define MAX_INVALIDATIONS 1024

typedef struct {
    uint64_t phys_addr;
    uint64_t timestamp;
    uint8_t data[CACHELINE_SIZE];
    struct back_invalidation *next;
} back_invalidation_t;

typedef struct {
    uint32_t head;
    uint32_t tail;
    pthread_mutex_t mutex;
    sem_t sem_items;
    back_invalidation_t entries[MAX_INVALIDATIONS];
} invalidation_queue_t;

// Global state for kbd hook
static uint64_t (*orig_kbd_read_data)(void *opaque, uint64_t addr, unsigned size) = NULL;
static invalidation_queue_t *inv_queue = NULL;
static int shm_fd = -1;

// Initialize shared memory for invalidation queue
static int init_kbd_shared_memory(void) {
    // Create or open shared memory
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("kbd_hook: shm_open failed");
        return -1;
    }
    
    // Set size
    if (ftruncate(shm_fd, SHM_SIZE) < 0) {
        perror("kbd_hook: ftruncate failed");
        close(shm_fd);
        return -1;
    }
    
    // Map shared memory
    inv_queue = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (inv_queue == MAP_FAILED) {
        perror("kbd_hook: mmap failed");
        close(shm_fd);
        return -1;
    }
    
    // Initialize if first time
    static int initialized = 0;
    if (!initialized) {
        initialized = 1;
        inv_queue->head = 0;
        inv_queue->tail = 0;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&inv_queue->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        
        sem_init(&inv_queue->sem_items, 1, 0);
    }
    
    fprintf(stderr, "kbd_hook: Shared memory initialized at %p\n", inv_queue);
    return 0;
}

// Check if an address has pending invalidations in shared memory queue
static int check_and_apply_invalidation(uint64_t phys_addr, void *data, size_t size) {
    if (!inv_queue) return 0;
    
    int found = 0;
    uint64_t cacheline_addr = phys_addr & ~(CACHELINE_SIZE - 1);
    
    pthread_mutex_lock(&inv_queue->mutex);
    
    uint32_t current = inv_queue->head;
    
    while (current != inv_queue->tail) {
        back_invalidation_t *entry = &inv_queue->entries[current];
        uint64_t inv_cacheline = entry->phys_addr & ~(CACHELINE_SIZE - 1);
        
        if (cacheline_addr == inv_cacheline) {
            // Apply invalidation
            if (data) {
                uint64_t offset = phys_addr & (CACHELINE_SIZE - 1);
                size_t copy_size = (size + offset > CACHELINE_SIZE) ? 
                                  (CACHELINE_SIZE - offset) : size;
                memcpy(data, entry->data + offset, copy_size);
            }
            
            fprintf(stderr, "kbd_hook: Applied invalidation for PA 0x%lx\n", phys_addr);
            
            // Remove from queue by advancing head
            if (current == inv_queue->head) {
                inv_queue->head = (inv_queue->head + 1) % MAX_INVALIDATIONS;
            }
            found = 1;
            break;
        }
        
        current = (current + 1) % MAX_INVALIDATIONS;
    }
    
    pthread_mutex_unlock(&inv_queue->mutex);
    
    return found;
}

// Direct invalidation check with CXLMemSim daemon
int cxlmemsim_check_invalidation(uint64_t phys_addr, size_t size, void *data) {
    // Check shared memory queue
    return check_and_apply_invalidation(phys_addr, data, size);
}

// Register invalidation for an address
void cxlmemsim_register_invalidation(uint64_t phys_addr, void *data, size_t size) {
    if (!inv_queue) return;
    
    pthread_mutex_lock(&inv_queue->mutex);
    
    uint32_t next_tail = (inv_queue->tail + 1) % MAX_INVALIDATIONS;
    if (next_tail != inv_queue->head) {
        back_invalidation_t *entry = &inv_queue->entries[inv_queue->tail];
        entry->phys_addr = phys_addr;
        entry->timestamp = get_timestamp_ns();
        if (data && size <= CACHELINE_SIZE) {
            memcpy(entry->data, data, size);
        }
        inv_queue->tail = next_tail;
        sem_post(&inv_queue->sem_items);
        
        fprintf(stderr, "kbd_hook: Registered invalidation for PA 0x%lx\n", phys_addr);
    }
    
    pthread_mutex_unlock(&inv_queue->mutex);
}

// Hook for kbd_read_data
uint64_t kbd_read_data(void *opaque, uint64_t addr, unsigned size) {
    static int initialized = 0;
    
    if (!initialized) {
        initialized = 1;
        
        // Get original function
        orig_kbd_read_data = dlsym(RTLD_NEXT, "kbd_read_data");
        if (!orig_kbd_read_data) {
            fprintf(stderr, "kbd_hook: Failed to find original kbd_read_data\n");
            exit(1);
        }
        
        // Initialize shared memory
        if (init_kbd_shared_memory() < 0) {
            fprintf(stderr, "kbd_hook: Failed to initialize shared memory\n");
        }
    }
    
    // Check for back invalidations
    uint8_t inv_data[8] = {0};
    int invalidated = 0;
    
    // Check shared memory queue
    invalidated = check_and_apply_invalidation(addr, inv_data, size);
    
    // Call original function
    uint64_t result = orig_kbd_read_data(opaque, addr, size);
    
    // Apply invalidated data if found
    if (invalidated && size <= sizeof(result)) {
        memcpy(&result, inv_data, size);
        fprintf(stderr, "kbd_hook: Using invalidated data for addr 0x%lx: 0x%lx\n", 
                addr, result);
    }
    
    // Log access if debug enabled
    if (getenv("KBD_HOOK_DEBUG")) {
        fprintf(stderr, "kbd_hook: read(0x%lx, %u) = 0x%lx %s\n",
                addr, size, result, invalidated ? "[INV]" : "");
    }
    
    return result;
}

// Cleanup kbd hook resources
void cleanup_kbd_hook(void) {
    // Unmap shared memory
    if (inv_queue) {
        munmap(inv_queue, SHM_SIZE);
        inv_queue = NULL;
    }
    
    // Close shared memory
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
    }
}