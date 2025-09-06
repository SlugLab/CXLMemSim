/*
 * Shared Memory Manager for CXLMemSim
 * Provides real shared memory allocation for CXL memory simulation
 * 
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 */

#ifndef SHARED_MEMORY_MANAGER_H
#define SHARED_MEMORY_MANAGER_H

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <set>
#include <shared_mutex>
#include <spdlog/spdlog.h>

// Cacheline size
#define CACHELINE_SIZE 64
#define CACHELINE_MASK (~(CACHELINE_SIZE - 1))

// Coherency states (must match main_server.cc)
enum CoherencyState {
    INVALID,
    SHARED,
    EXCLUSIVE,
    MODIFIED
};

// Cacheline metadata (stored separately from data)
struct CachelineMetadata {
    CoherencyState state;
    std::set<int> sharers;      // Thread IDs that have this cacheline in SHARED state
    int owner;                   // Thread ID that owns in EXCLUSIVE/MODIFIED state
    uint64_t last_access_time;
    bool has_dirty_update;       // Flag for back invalidation
    uint64_t dirty_update_time;  // Timestamp of dirty update
    uint64_t version;           // Version number for consistency
    std::mutex lock;            // Per-cacheline lock
    
    CachelineMetadata() : state(INVALID), owner(-1), last_access_time(0), 
                          has_dirty_update(false), dirty_update_time(0), version(0) {}
};

class SharedMemoryManager {
private:
    // Shared memory info
    std::string shm_name;
    int shm_fd;
    void* shm_base;
    size_t shm_size;
    size_t capacity_mb;
    // Optional file backing instead of POSIX shm
    bool use_file_backing = false;
    std::string backing_file_path;
    
    // Memory layout:
    // [Header][Cacheline Data Area][Metadata Area]
    struct SharedMemoryHeader {
        uint64_t magic;         // Magic number for validation
        uint64_t version;       // Format version
        size_t total_size;      // Total shared memory size
        size_t data_offset;     // Offset to cacheline data area
        size_t metadata_offset; // Offset to metadata area
        uint64_t num_cachelines;// Number of cachelines
        uint64_t base_addr;     // Base physical address for CXL memory
    };
    
    SharedMemoryHeader* header;
    uint8_t* data_area;         // Pointer to cacheline data area
    
    // Local metadata cache (not in shared memory)
    std::map<uint64_t, std::unique_ptr<CachelineMetadata>> metadata_cache;
    std::shared_mutex metadata_mutex;
    
    // Memory region tracking
    struct MemoryRegion {
        uint64_t base_addr;
        size_t size;
        bool allocated;
    };
    std::vector<MemoryRegion> regions;
    
public:
    explicit SharedMemoryManager(size_t capacity_mb, const std::string& shm_name = "/cxlmemsim_shared");
    SharedMemoryManager(size_t capacity_mb, const std::string& shm_name, bool use_file, const std::string& file_path);
    ~SharedMemoryManager();
    
    // Initialize shared memory
    bool initialize();
    void cleanup();
    
    // Get shared memory info for clients
    struct SharedMemoryInfo {
        std::string shm_name;
        size_t size;
        uint64_t base_addr;
        size_t num_cachelines;
    };
    SharedMemoryInfo get_shm_info() const;
    
    // Cacheline data access (direct to shared memory)
    uint8_t* get_cacheline_data(uint64_t cacheline_addr);
    bool read_cacheline(uint64_t addr, uint8_t* buffer, size_t size);
    bool write_cacheline(uint64_t addr, const uint8_t* data, size_t size);
    
    // Metadata access (uses local cache)
    CachelineMetadata* get_cacheline_metadata(uint64_t cacheline_addr);
    
    // Memory region management
    bool allocate_region(uint64_t addr, size_t size);
    bool deallocate_region(uint64_t addr);
    bool is_valid_address(uint64_t addr) const;
    
    // Utility functions
    uint64_t addr_to_cacheline(uint64_t addr) const {
        return addr & CACHELINE_MASK;
    }
    
    uint64_t cacheline_to_index(uint64_t cacheline_addr) const {
        if (header && header->base_addr == 0) {
            // Accept any address, use modulo to map to available cachelines
            return (cacheline_addr / CACHELINE_SIZE) % header->num_cachelines;
        }
        return header ? (cacheline_addr - header->base_addr) / CACHELINE_SIZE : 0;
    }
    
    // Statistics
    struct MemoryStats {
        size_t total_capacity;
        size_t used_memory;
        uint64_t num_cachelines;
        uint64_t active_cachelines;
    };
    MemoryStats get_stats() const;
    
private:
    bool create_shared_memory();
    bool create_file_backing();
    bool map_shared_memory();
    void initialize_header();
    void initialize_data_area();
};

#endif // SHARED_MEMORY_MANAGER_H
