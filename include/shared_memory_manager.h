/*
 * Shared Memory Manager for CXLMemSim
 * Provides real shared memory allocation for CXL memory simulation
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 */

#ifndef SHARED_MEMORY_MANAGER_H
#define SHARED_MEMORY_MANAGER_H

#include <cstdint>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

class SsdStreamingBackend;

// Cacheline size
#define SHM_CACHELINE_SIZE (64)
#define SHM_CACHELINE_MASK (~(SHM_CACHELINE_SIZE - 1))

// Coherency states (must match main_server.cc)
enum CoherencyState { INVALID, SHARED, EXCLUSIVE, MODIFIED };

// Cacheline metadata (stored separately from data)
struct CachelineMetadata {
    CoherencyState state;
    std::set<int> sharers;
    // Thread IDs that have this cacheline in SHARED state
    int owner;
    // Thread ID that owns in EXCLUSIVE/MODIFIED state
    uint64_t last_access_time;
    bool has_dirty_update;
    // Flag for back invalidation
    uint64_t dirty_update_time; // Timestamp of dirty update
    uint64_t version;
    // Version number for consistency
    std::mutex lock;
    // Per-cacheline lock

    CachelineMetadata()
        : state(INVALID), owner(-1), last_access_time(0), has_dirty_update(false), dirty_update_time(0), version(0) {}
};

class SharedMemoryManager {
public:
    enum class BackingMode {
        SharedMemory,
        FileMmap,
        SsdStream,
    };

    struct SsdStreamingConfig {
        std::string backing_path;
        uint64_t capacity_bytes = 0;
        size_t page_size = 4096;
        size_t io_chunk_size = 64 * 1024;
        size_t cache_pages = 4096;
        size_t read_ahead_pages = 16;
        bool use_io_uring = true;
        bool use_odirect = true;
    };

private:
    // Shared memory info
    std::string shm_name;
    int shm_fd;
    void *shm_base;
    size_t shm_size;
    size_t capacity_mb;
    // Optional file backing instead of POSIX shm
    BackingMode backing_mode = BackingMode::SharedMemory;
    bool use_file_backing = false;
    std::string backing_file_path;
    SsdStreamingConfig ssd_config;
#ifdef CXLMEMSIM_HAS_SSD_STREAMING_BACKEND
    std::unique_ptr<SsdStreamingBackend> ssd_backend;
#endif

    // Memory layout:
    // [Header][Cacheline Data Area][Metadata Area]
    struct SharedMemoryHeader {
        uint64_t magic;
        // Magic number for validation
        uint64_t version;
        // Format version
        size_t total_size;
        // Total shared memory size
        size_t data_offset;
        // Offset to cacheline data area
        size_t metadata_offset; // Offset to metadata area
        uint64_t num_cachelines; // Number of cachelines
        uint64_t base_addr;
        // Base physical address for CXL memory
    };

    SharedMemoryHeader header_storage{};
    SharedMemoryHeader *header;
    uint8_t *data_area;
    // Pointer to cacheline data area

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
    explicit SharedMemoryManager(size_t capacity_mb, const std::string &shm_name = "/cxlmemsim_pgas");
    SharedMemoryManager(size_t capacity_mb, const std::string &shm_name, bool use_file, const std::string &file_path);
    SharedMemoryManager(size_t capacity_mb, const std::string &shm_name, const SsdStreamingConfig &ssd_config);
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
    uint8_t *get_cacheline_data(uint64_t cacheline_addr);
    bool read_cacheline(uint64_t addr, uint8_t *buffer, size_t size);
    bool write_cacheline(uint64_t addr, const uint8_t *data, size_t size);
    bool atomic_fetch_add_uint64(uint64_t addr, uint64_t value, uint64_t *old_value);
    bool atomic_compare_exchange_uint64(uint64_t addr, uint64_t expected, uint64_t desired, uint64_t *old_value);
    bool flush();
    bool flush_cacheline(uint64_t addr, size_t size);

    // Direct access to data area (for msync)
    void *get_data_area() { return data_area; }
    bool has_direct_data_area() const { return data_area != nullptr; }
    bool is_ssd_streaming() const { return backing_mode == BackingMode::SsdStream; }

    // Streaming hint placeholders. Backends that do not support hints treat them as no-ops.
    bool prefetch(uint64_t addr, size_t size);
    bool evict_after(uint64_t addr, size_t size);
    bool pin(uint64_t addr, size_t size);
    bool unpin(uint64_t addr, size_t size);
    void set_streaming(bool enabled);

    // Metadata access (uses local cache)
    CachelineMetadata *get_cacheline_metadata(uint64_t cacheline_addr);

    // Set the base address (for distributed mode where each node needs a unique range)
    void set_base_addr(uint64_t addr);

    // Memory region management
    bool allocate_region(uint64_t addr, size_t size);
    bool deallocate_region(uint64_t addr);
    bool is_valid_address(uint64_t addr) const;

    // Utility functions
    uint64_t addr_to_cacheline(uint64_t addr) const { return addr & SHM_CACHELINE_MASK; }

    uint64_t cacheline_to_index(uint64_t cacheline_addr) const {
        if (!header || header->num_cachelines == 0) {
            return 0;
        }
        if (header && header->base_addr == 0) {
            // Accept any address, use modulo to map to available cachelines
            return (cacheline_addr / SHM_CACHELINE_SIZE) % header->num_cachelines;
        }
        return (cacheline_addr - header->base_addr) / SHM_CACHELINE_SIZE;
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
    bool create_ssd_streaming_backend();
    bool map_shared_memory();
    void initialize_header();
    void initialize_data_area();
    size_t data_capacity_bytes() const;
    bool address_to_backend_offset(uint64_t addr, size_t access_size, uint64_t *offset) const;
};

#endif // SHARED_MEMORY_MANAGER_H
