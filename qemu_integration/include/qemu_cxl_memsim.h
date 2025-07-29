#ifndef QEMU_CXL_MEMSIM_H
#define QEMU_CXL_MEMSIM_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CACHELINE_SIZE 64
#define CXL_READ_OP 0
#define CXL_WRITE_OP 1

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

int cxlmemsim_init(const char *host, int port);
void cxlmemsim_cleanup(void);

int cxl_type3_read(uint64_t addr, void *data, size_t size);
int cxl_type3_write(uint64_t addr, const void *data, size_t size);

uint64_t cxlmemsim_get_hotness(uint64_t addr);
void cxlmemsim_dump_hotness_stats(void);

// Back invalidation support for keyboard hook
int cxlmemsim_check_invalidation(uint64_t phys_addr, size_t size, void *data);
void cxlmemsim_register_invalidation(uint64_t phys_addr, void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif