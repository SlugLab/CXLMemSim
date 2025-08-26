#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <numa.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>

#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_MAGENTA "\x1b[35m"
#define COLOR_CYAN    "\x1b[36m"
#define COLOR_RESET   "\x1b[0m"

// Function to read /proc/self/maps to see memory mappings
void print_memory_mappings() {
    printf("\n" COLOR_CYAN "=== Current Memory Mappings ===" COLOR_RESET "\n");
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[256];
        int count = 0;
        while (fgets(line, sizeof(line), maps) && count < 20) {
            printf("%s", line);
            count++;
        }
        if (count >= 20) {
            printf("... (truncated, showing first 20 mappings)\n");
        }
        fclose(maps);
    }
}

// Function to read NUMA memory statistics
void print_numa_stats() {
    printf("\n" COLOR_CYAN "=== NUMA Memory Statistics ===" COLOR_RESET "\n");
    
    if (numa_available() < 0) {
        printf(COLOR_RED "NUMA not available\n" COLOR_RESET);
        return;
    }
    
    int num_nodes = numa_num_configured_nodes();
    printf("Number of NUMA nodes: %d\n", num_nodes);
    
    for (int node = 0; node < num_nodes; node++) {
        long free_size = numa_node_size64(node, NULL);
        printf("\nNode %d:\n", node);
        printf("  Total size: %.2f GB\n", free_size / (1024.0 * 1024.0 * 1024.0));
        
        // Check if node is online
        if (numa_bitmask_isbitset(numa_all_nodes_ptr, node)) {
            printf("  Status: " COLOR_GREEN "Online" COLOR_RESET "\n");
        } else {
            printf("  Status: " COLOR_RED "Offline" COLOR_RESET "\n");
        }
        
        // Get CPU affinity for this node
        struct bitmask *cpus = numa_allocate_cpumask();
        if (numa_node_to_cpus(node, cpus) == 0) {
            printf("  CPUs: ");
            int first = 1;
            for (int cpu = 0; cpu < numa_num_configured_cpus(); cpu++) {
                if (numa_bitmask_isbitset(cpus, cpu)) {
                    if (!first) printf(",");
                    printf("%d", cpu);
                    first = 0;
                }
            }
            printf("\n");
        }
        numa_bitmask_free(cpus);
    }
}

// Function to test memory allocation and access patterns
void test_memory_access(int target_node, size_t size) {
    printf("\n" COLOR_CYAN "=== Testing Memory Access on Node %d ===" COLOR_RESET "\n", target_node);
    
    if (target_node >= numa_num_configured_nodes()) {
        printf(COLOR_RED "Error: Node %d does not exist\n" COLOR_RESET, target_node);
        return;
    }
    
    // Allocate memory on specific node
    void *mem = numa_alloc_onnode(size, target_node);
    if (!mem) {
        printf(COLOR_RED "Failed to allocate %zu bytes on node %d\n" COLOR_RESET, size, target_node);
        return;
    }
    
    printf(COLOR_GREEN "Successfully allocated %zu bytes on node %d\n" COLOR_RESET, size, target_node);
    printf("Memory address: %p\n", mem);
    
    // Write pattern
    printf("\nWriting test pattern...\n");
    clock_t start = clock();
    for (size_t i = 0; i < size; i += 64) {
        ((volatile char*)mem)[i] = (char)(i & 0xFF);
    }
    clock_t end = clock();
    double write_time = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("Write time: %.4f seconds\n", write_time);
    printf("Write bandwidth: %.2f MB/s\n", (size / (1024.0 * 1024.0)) / write_time);
    
    // Read pattern
    printf("\nReading and verifying pattern...\n");
    start = clock();
    int errors = 0;
    for (size_t i = 0; i < size; i += 64) {
        char expected = (char)(i & 0xFF);
        char actual = ((volatile char*)mem)[i];
        if (actual != expected && errors < 10) {
            printf(COLOR_RED "Error at offset %zu: expected 0x%02x, got 0x%02x\n" COLOR_RESET,
                   i, expected & 0xFF, actual & 0xFF);
            errors++;
        }
    }
    end = clock();
    double read_time = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("Read time: %.4f seconds\n", read_time);
    printf("Read bandwidth: %.2f MB/s\n", (size / (1024.0 * 1024.0)) / read_time);
    
    if (errors == 0) {
        printf(COLOR_GREEN "✓ Memory verification passed\n" COLOR_RESET);
    } else {
        printf(COLOR_RED "✗ Memory verification failed with %d errors\n" COLOR_RESET, errors);
    }
    
    // Check actual node placement
    int actual_node = -1;
    if (numa_move_pages(0, 1, &mem, NULL, &actual_node, 0) == 0) {
        if (actual_node == target_node) {
            printf(COLOR_GREEN "✓ Memory is on requested node %d\n" COLOR_RESET, actual_node);
        } else {
            printf(COLOR_YELLOW "⚠ Memory is on node %d (requested %d)\n" COLOR_RESET, 
                   actual_node, target_node);
        }
    }
    
    numa_free(mem, size);
}

// Function to read memory bandwidth using streaming access
void benchmark_memory_bandwidth(int node, size_t size) {
    printf("\n" COLOR_CYAN "=== Bandwidth Benchmark for Node %d ===" COLOR_RESET "\n", node);
    
    void *mem = numa_alloc_onnode(size, node);
    if (!mem) {
        printf(COLOR_RED "Failed to allocate memory for benchmark\n" COLOR_RESET);
        return;
    }
    
    // Sequential write
    printf("Sequential write test...\n");
    clock_t start = clock();
    memset(mem, 0x42, size);
    clock_t end = clock();
    double write_time = ((double)(end - start)) / CLOCKS_PER_SEC;
    double write_bw = (size / (1024.0 * 1024.0 * 1024.0)) / write_time;
    printf("  Write bandwidth: %.2f GB/s\n", write_bw);
    
    // Sequential read
    printf("Sequential read test...\n");
    volatile long sum = 0;
    start = clock();
    for (size_t i = 0; i < size; i += sizeof(long)) {
        sum += *((long*)((char*)mem + i));
    }
    end = clock();
    double read_time = ((double)(end - start)) / CLOCKS_PER_SEC;
    double read_bw = (size / (1024.0 * 1024.0 * 1024.0)) / read_time;
    printf("  Read bandwidth: %.2f GB/s\n", read_bw);
    printf("  (Checksum: %ld)\n", sum); // Prevent optimization
    
    numa_free(mem, size);
}

// Function to display memory hierarchy info
void display_memory_info() {
    printf("\n" COLOR_CYAN "=== System Memory Information ===" COLOR_RESET "\n");
    
    // Total system memory
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    printf("Total system memory: %.2f GB\n", 
           (pages * page_size) / (1024.0 * 1024.0 * 1024.0));
    
    // Available memory
    long avail_pages = sysconf(_SC_AVPHYS_PAGES);
    printf("Available memory: %.2f GB\n", 
           (avail_pages * page_size) / (1024.0 * 1024.0 * 1024.0));
    
    // Page size
    printf("Page size: %ld bytes\n", page_size);
    
    // Check transparent huge pages
    FILE *thp = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    if (thp) {
        char buffer[256];
        if (fgets(buffer, sizeof(buffer), thp)) {
            printf("Transparent Huge Pages: %s", buffer);
        }
        fclose(thp);
    }
}

// Function to monitor CXL-specific information if available
void check_cxl_info() {
    printf("\n" COLOR_CYAN "=== CXL Device Information ===" COLOR_RESET "\n");
    
    // Check for CXL devices in /sys/bus/cxl/devices/
    system("ls /sys/bus/cxl/devices/ 2>/dev/null | head -5");
    
    // Check dmesg for CXL messages
    printf("\nRecent CXL-related kernel messages:\n");
    system("dmesg | grep -i cxl | tail -5");
    
    // Check for DAX devices (often used with CXL)
    printf("\nDAX devices:\n");
    system("ls /dev/dax* 2>/dev/null");
}

int main(int argc, char *argv[]) {
    printf(COLOR_MAGENTA "\n========================================\n");
    printf("     CXL Memory Reader & Analyzer\n");
    printf("========================================\n" COLOR_RESET);
    
    // Display system information
    display_memory_info();
    print_numa_stats();
    
    // Check for CXL devices
    check_cxl_info();
    
    // Parse command line arguments
    int test_node = 1;  // Default to node 1 (typically CXL)
    size_t test_size = 16 * 1024 * 1024;  // Default 16MB
    
    if (argc > 1) {
        test_node = atoi(argv[1]);
    }
    if (argc > 2) {
        test_size = atoll(argv[2]) * 1024 * 1024;  // Convert MB to bytes
    }
    
    printf("\n" COLOR_YELLOW "Test Configuration:" COLOR_RESET "\n");
    printf("  Target NUMA node: %d\n", test_node);
    printf("  Test size: %zu MB\n", test_size / (1024 * 1024));
    
    // Run memory tests
    if (numa_available() >= 0) {
        test_memory_access(test_node, test_size);
        benchmark_memory_bandwidth(test_node, test_size);
        
        // Compare with node 0 (typically DRAM)
        if (test_node != 0 && numa_num_configured_nodes() > 1) {
            printf("\n" COLOR_YELLOW "=== Comparing with Node 0 (DRAM) ===" COLOR_RESET "\n");
            benchmark_memory_bandwidth(0, test_size);
        }
    }
    
    // Show memory mappings (abbreviated)
    print_memory_mappings();
    
    printf("\n" COLOR_GREEN "Analysis complete!" COLOR_RESET "\n");
    printf("\nUsage: %s [node_number] [size_in_MB]\n", argv[0]);
    printf("Example: %s 1 64  # Test 64MB on NUMA node 1\n", argv[0]);
    
    return 0;
}