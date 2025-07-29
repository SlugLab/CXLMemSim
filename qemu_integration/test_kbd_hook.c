#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

// Simulate kbd_read_data for testing
uint64_t kbd_read_data(void *opaque, uint64_t addr, unsigned size) {
    static uint64_t counter = 0;
    printf("Original kbd_read_data called: addr=0x%lx, size=%u\n", addr, size);
    return ++counter;
}

// Test invalidation by calling the CXLMemSim function
void trigger_invalidation(uint64_t phys_addr, uint8_t *data, size_t size) {
    // Use the public API to register invalidation
    void (*register_inv)(uint64_t, void*, size_t) = dlsym(RTLD_DEFAULT, "cxlmemsim_register_invalidation");
    if (register_inv) {
        printf("Triggering invalidation for PA 0x%lx\n", phys_addr);
        register_inv(phys_addr, data, size);
    } else {
        printf("Warning: cxlmemsim_register_invalidation not found\n");
    }
}

int main() {
    printf("Testing keyboard hook with back invalidation\n");
    printf("Make sure to run with: LD_PRELOAD=./libCXLMemSim.so ./test_kbd_hook\n\n");
    
    // Test normal read
    uint64_t result1 = kbd_read_data(NULL, 0x1000, 8);
    printf("Read 1 result: 0x%lx\n\n", result1);
    
    // Trigger invalidation for address 0x2000
    uint8_t invalid_data[64] = {0};
    memset(invalid_data, 0xAA, sizeof(invalid_data));
    trigger_invalidation(0x2000, invalid_data, 64);
    
    sleep(1); // Give time for invalidation to process
    
    // Read from invalidated address
    uint64_t result2 = kbd_read_data(NULL, 0x2000, 8);
    printf("Read 2 result (should be invalidated): 0x%lx\n\n", result2);
    
    // Read from non-invalidated address
    uint64_t result3 = kbd_read_data(NULL, 0x3000, 8);
    printf("Read 3 result (normal): 0x%lx\n", result3);
    
    return 0;
}