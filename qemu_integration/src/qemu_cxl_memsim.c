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

int cxl_type3_read(uint64_t addr, void *data, size_t size) {
    if (!g_ctx) {
        fprintf(stderr, "CXLMemSim not initialized\n");
        return -1;
    }
    
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

int cxl_type3_write(uint64_t addr, const void *data, size_t size) {
    if (!g_ctx) {
        fprintf(stderr, "CXLMemSim not initialized\n");
        return -1;
    }
    
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