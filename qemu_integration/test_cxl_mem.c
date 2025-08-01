#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>

#define CXL_MEM_SIZE (1UL << 30)  // 1GB
#define CACHE_LINE_SIZE 64
#define NUM_ITERATIONS 1000000
#define TEST_OFFSET 0x1000  // Test at 4KB offset

// Structure to pass data to threads
struct thread_data {
    int host_id;
    volatile uint64_t *shared_addr;
    uint64_t iterations;
    uint64_t conflicts_detected;
};

// Function to perform cache line writes from a host
void* host_writer(void* arg) {
    struct thread_data *data = (struct thread_data*)arg;
    volatile uint64_t *addr = data->shared_addr;
    uint64_t expected_value = data->host_id;
    uint64_t conflicts = 0;
    
    printf("Host %d starting write operations at address %p\n", data->host_id, addr);
    
    for (uint64_t i = 0; i < data->iterations; i++) {
        // Write host-specific pattern to entire cache line
        for (int j = 0; j < CACHE_LINE_SIZE / sizeof(uint64_t); j++) {
            addr[j] = expected_value;
        }
        
        // Memory barrier to ensure write completion
        __sync_synchronize();
        
        // Read back and check for conflicts
        for (int j = 0; j < CACHE_LINE_SIZE / sizeof(uint64_t); j++) {
            uint64_t read_value = addr[j];
            if (read_value != expected_value) {
                conflicts++;
                break;  // Conflict detected in this iteration
            }
        }
        
        // Small delay to increase conflict probability
        if (i % 1000 == 0) {
            usleep(1);
        }
    }
    
    data->conflicts_detected = conflicts;
    printf("Host %d completed. Conflicts detected: %lu / %lu (%.2f%%)\n", 
           data->host_id, conflicts, data->iterations, 
           (double)conflicts / data->iterations * 100);
    
    return NULL;
}

int main(int argc, char *argv[]) {
    int fd;
    void *cxl_mem_base;
    const char *cxl_dev_path = "/dev/dax0.0";  // Default CXL device path
    
    if (argc > 1) {
        cxl_dev_path = argv[1];
    }
    
    printf("CXL Memory Cache Line Conflict Test\n");
    printf("===================================\n");
    printf("Using CXL device: %s\n", cxl_dev_path);
    printf("Cache line size: %d bytes\n", CACHE_LINE_SIZE);
    printf("Test iterations: %d\n", NUM_ITERATIONS);
    
    // Open CXL memory device
    fd = open(cxl_dev_path, O_RDWR);
    if (fd < 0) {
        perror("Failed to open CXL device");
        return 1;
    }
    
    // Map CXL memory
    cxl_mem_base = mmap(NULL, CXL_MEM_SIZE, PROT_READ | PROT_WRITE, 
                        MAP_SHARED, fd, 0);
    if (cxl_mem_base == MAP_FAILED) {
        perror("Failed to map CXL memory");
        close(fd);
        return 1;
    }
    
    printf("Mapped %lu bytes of CXL memory at %p\n", CXL_MEM_SIZE, cxl_mem_base);
    
    // Calculate aligned address for cache line test
    volatile uint64_t *test_addr = (volatile uint64_t*)((char*)cxl_mem_base + TEST_OFFSET);
    
    // Ensure address is cache line aligned
    if ((uintptr_t)test_addr % CACHE_LINE_SIZE != 0) {
        test_addr = (volatile uint64_t*)(((uintptr_t)test_addr + CACHE_LINE_SIZE - 1) 
                    & ~(CACHE_LINE_SIZE - 1));
    }
    
    printf("Test address (cache line aligned): %p\n", test_addr);
    
    // Initialize the cache line
    memset((void*)test_addr, 0, CACHE_LINE_SIZE);
    
    // Create thread data for two hosts
    struct thread_data host1_data = {
        .host_id = 1,
        .shared_addr = test_addr,
        .iterations = NUM_ITERATIONS,
        .conflicts_detected = 0
    };
    
    struct thread_data host2_data = {
        .host_id = 2,
        .shared_addr = test_addr,
        .iterations = NUM_ITERATIONS,
        .conflicts_detected = 0
    };
    
    // Create threads to simulate two hosts
    pthread_t host1_thread, host2_thread;
    
    printf("\nStarting cache line conflict test with two hosts...\n");
    
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    if (pthread_create(&host1_thread, NULL, host_writer, &host1_data) != 0) {
        perror("Failed to create host1 thread");
        munmap(cxl_mem_base, CXL_MEM_SIZE);
        close(fd);
        return 1;
    }
    
    if (pthread_create(&host2_thread, NULL, host_writer, &host2_data) != 0) {
        perror("Failed to create host2 thread");
        pthread_cancel(host1_thread);
        munmap(cxl_mem_base, CXL_MEM_SIZE);
        close(fd);
        return 1;
    }
    
    // Wait for both threads to complete
    pthread_join(host1_thread, NULL);
    pthread_join(host2_thread, NULL);
    
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    
    // Calculate elapsed time
    double elapsed = (end_time.tv_sec - start_time.tv_sec) + 
                     (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
    
    // Print summary
    printf("\n=== Test Summary ===\n");
    printf("Total test time: %.3f seconds\n", elapsed);
    printf("Host 1 conflicts: %lu / %lu (%.2f%%)\n", 
           host1_data.conflicts_detected, host1_data.iterations,
           (double)host1_data.conflicts_detected / host1_data.iterations * 100);
    printf("Host 2 conflicts: %lu / %lu (%.2f%%)\n", 
           host2_data.conflicts_detected, host2_data.iterations,
           (double)host2_data.conflicts_detected / host2_data.iterations * 100);
    printf("Total conflicts: %lu\n", 
           host1_data.conflicts_detected + host2_data.conflicts_detected);
    
    // Additional coherency verification
    printf("\nPerforming final coherency check...\n");
    uint64_t final_value = test_addr[0];
    int coherent = 1;
    
    for (int i = 0; i < CACHE_LINE_SIZE / sizeof(uint64_t); i++) {
        if (test_addr[i] != final_value) {
            coherent = 0;
            printf("Coherency issue at offset %d: expected %lu, got %lu\n",
                   i * 8, final_value, test_addr[i]);
        }
    }
    
    if (coherent) {
        printf("Final state is coherent. Last writer: Host %lu\n", final_value);
    } else {
        printf("WARNING: Final state shows coherency issues!\n");
    }
    
    // Cleanup
    munmap(cxl_mem_base, CXL_MEM_SIZE);
    close(fd);
    
    return 0;
}