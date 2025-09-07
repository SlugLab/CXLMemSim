/*
 * Example showing how the DAX open mode is used in the litmus tests
 * This demonstrates the key components from dax_litmus_common.h
 */

#include "dax_litmus_common.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <path:/dev/daxX.Y|shm> [size_MB] [offset]\n", argv[0]);
        printf("\nExamples:\n");
        printf("  %s /dev/dax0.0        # Use DAX device\n", argv[0]);
        printf("  %s shm                # Use CXLMemSim shared memory\n", argv[0]);
        printf("  %s /dev/dax0.0 16 0   # 16MB at offset 0\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    size_t size = (argc > 2) ? (strtoull(argv[2], NULL, 0) * 1024ULL * 1024ULL) : 2ULL * 1024 * 1024;
    size_t offset = (argc > 3) ? strtoull(argv[3], NULL, 0) : 0;

    printf("Opening: %s\n", path);
    printf("Size requested: %zu bytes\n", size);
    printf("Offset: %zu bytes\n", offset);
    printf("\n");

    // The map_handle_t structure tracks the mapping details
    map_handle_t mh;
    
    // map_region handles both DAX devices and CXLMemSim shared memory
    // Key features:
    // 1. For DAX: Opens with O_RDWR | O_CLOEXEC
    // 2. For shm: Opens "/cxlmemsim_shared" and skips header
    // 3. Returns pointer to usable region at specified offset
    void *region = map_region(path, &size, offset, &mh);
    
    if (!region) {
        fprintf(stderr, "Failed to map region\n");
        return 1;
    }

    printf("Successfully mapped region:\n");
    printf("  Base address: %p\n", mh.base);
    printf("  Mapped size: %zu bytes\n", mh.map_size);
    printf("  Data pointer: %p\n", region);
    printf("  Is SHM: %s\n", mh.is_shm ? "yes" : "no");
    if (mh.is_shm) {
        printf("  SHM data offset: %zu bytes (header skip)\n", mh.data_off);
    }
    printf("\n");

    // Example: Use the mapped region
    ctrl_block_t *ctrl = (ctrl_block_t *)region;
    
    // Initialize control block
    atomic_store_explicit(&ctrl->magic, 0xDEADBEEF, memory_order_relaxed);
    atomic_store_explicit(&ctrl->ready_a, 0, memory_order_relaxed);
    atomic_store_explicit(&ctrl->ready_b, 0, memory_order_relaxed);
    atomic_store_explicit(&ctrl->seq, 0, memory_order_relaxed);
    atomic_store_explicit(&ctrl->flag, 0, memory_order_relaxed);
    atomic_store_explicit(&ctrl->counter, 0, memory_order_relaxed);
    
    printf("Initialized control block at %p:\n", ctrl);
    printf("  magic: 0x%x\n", atomic_load_explicit(&ctrl->magic, memory_order_relaxed));
    printf("  ready_a: %u\n", atomic_load_explicit(&ctrl->ready_a, memory_order_relaxed));
    printf("  ready_b: %u\n", atomic_load_explicit(&ctrl->ready_b, memory_order_relaxed));
    printf("  counter: %lu\n", atomic_load_explicit(&ctrl->counter, memory_order_relaxed));
    printf("\n");

    // Write test pattern to data area
    uint8_t *data_area = (uint8_t *)region + sizeof(ctrl_block_t);
    const char *test_msg = "Hello from DAX/CXL memory!";
    strcpy((char *)data_area, test_msg);
    
    printf("Wrote test message at offset %zu: '%s'\n", sizeof(ctrl_block_t), test_msg);
    
    // Read it back to verify
    char read_buffer[100];
    strncpy(read_buffer, (char *)data_area, 99);
    read_buffer[99] = '\0';
    printf("Read back: '%s'\n", read_buffer);
    printf("\n");

    // Show first 64 bytes as hex dump
    printf("First 64 bytes of data (hex):\n");
    for (int i = 0; i < 64; i++) {
        if (i % 16 == 0) printf("%04x: ", i);
        printf("%02x ", ((uint8_t *)region)[i]);
        if (i % 16 == 15) printf("\n");
    }
    printf("\n");

    // Clean up
    printf("Unmapping region...\n");
    unmap_region(&mh);
    
    printf("Done!\n");
    return 0;
}