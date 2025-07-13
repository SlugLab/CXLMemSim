#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "include/qemu_cxl_memsim.h"

#define TEST_SIZE 4096
#define NUM_ITERATIONS 1000

int main(int argc, char *argv[]) {
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? atoi(argv[2]) : 9999;
    
    printf("Testing CXLMemSim connection to %s:%d\n", host, port);
    
    if (cxlmemsim_init(host, port) < 0) {
        fprintf(stderr, "Failed to initialize CXLMemSim\n");
        return 1;
    }
    
    uint8_t *write_buffer = malloc(TEST_SIZE);
    uint8_t *read_buffer = malloc(TEST_SIZE);
    
    for (int i = 0; i < TEST_SIZE; i++) {
        write_buffer[i] = i & 0xFF;
    }
    
    printf("\nTesting sequential access pattern...\n");
    clock_t start = clock();
    
    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        uint64_t addr = (iter * 4096) % (1024 * 1024 * 1024);
        
        if (cxl_type3_write(addr, write_buffer, TEST_SIZE) < 0) {
            fprintf(stderr, "Write failed at iteration %d\n", iter);
            break;
        }
        
        if (cxl_type3_read(addr, read_buffer, TEST_SIZE) < 0) {
            fprintf(stderr, "Read failed at iteration %d\n", iter);
            break;
        }
        
        if (memcmp(write_buffer, read_buffer, TEST_SIZE) != 0) {
            fprintf(stderr, "Data mismatch at iteration %d\n", iter);
            break;
        }
        
        if ((iter + 1) % 100 == 0) {
            printf("Completed %d iterations\n", iter + 1);
        }
    }
    
    clock_t end = clock();
    double elapsed = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("Sequential test completed in %.2f seconds\n", elapsed);
    
    printf("\nTesting random access pattern (hotspot creation)...\n");
    start = clock();
    
    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        uint64_t hot_addr = 0x1000000;
        
        if (rand() % 100 < 80) {
            cxl_type3_read(hot_addr, read_buffer, 64);
        } else {
            uint64_t random_addr = (rand() % 1024) * 4096;
            cxl_type3_read(random_addr, read_buffer, 64);
        }
    }
    
    end = clock();
    elapsed = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("Random test completed in %.2f seconds\n", elapsed);
    
    printf("\nMemory hotness statistics:\n");
    cxlmemsim_dump_hotness_stats();
    
    free(write_buffer);
    free(read_buffer);
    cxlmemsim_cleanup();
    
    return 0;
}