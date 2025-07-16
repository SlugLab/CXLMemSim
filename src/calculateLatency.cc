/*
 * CXLMemSim Latency Calculator
 * A simple tool to calculate CXL memory access latency
 *
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 */

#include <iostream>
#include <cstdint>

// Simple latency calculation function that can be used by QEMU integration
extern "C" uint64_t cxlmemsim_calculate_latency(uint64_t addr, size_t size, bool is_read) {
    // Base latencies (in nanoseconds)
    const uint64_t base_read_latency = 150;
    const uint64_t base_write_latency = 100;
    const uint64_t cacheline_size = 64;
    
    // Start with base latency
    uint64_t latency = is_read ? base_read_latency : base_write_latency;
    
    // Add extra latency for larger transfers
    if (size > cacheline_size) {
        size_t num_cachelines = (size + cacheline_size - 1) / cacheline_size;
        latency += (num_cachelines - 1) * 10;
    }
    
    // Simple distance-based latency (based on address range)
    uint64_t gb_offset = addr / (1024ULL * 1024 * 1024);
    if (gb_offset > 4) {
        // Remote memory
        latency += 150;
    } else if (gb_offset > 2) {
        // Far local memory
        latency += 50;
    }
    
    return latency;
}

// Main function for testing
int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <address> <size> <read|write>" << std::endl;
        return 1;
    }
    
    uint64_t addr = std::stoull(argv[1], nullptr, 0);
    size_t size = std::stoull(argv[2], nullptr, 0);
    bool is_read = (std::string(argv[3]) == "read");
    
    uint64_t latency = cxlmemsim_calculate_latency(addr, size, is_read);
    
    std::cout << "Address: 0x" << std::hex << addr << std::dec << std::endl;
    std::cout << "Size: " << size << " bytes" << std::endl;
    std::cout << "Operation: " << (is_read ? "read" : "write") << std::endl;
    std::cout << "Latency: " << latency << " ns" << std::endl;
    
    return 0;
}