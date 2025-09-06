/*
 * Shared Memory Manager Implementation for CXLMemSim
 * 
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 */

#include "shared_memory_manager.h"
#include <cstring>
#include <stdexcept>
#include <sys/types.h>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define MAGIC_NUMBER 0x43584C4D454D5348ULL  // "CXLMEMSH"
#define FORMAT_VERSION 1

SharedMemoryManager::SharedMemoryManager(size_t capacity_mb, const std::string& shm_name)
    : capacity_mb(capacity_mb), shm_name(shm_name), shm_fd(-1), shm_base(nullptr), 
      shm_size(0), header(nullptr), data_area(nullptr) {
    
    // Calculate sizes
    shm_size = capacity_mb * 1024 * 1024;
    
    // Reserve some space for header
    size_t header_size = sizeof(SharedMemoryHeader);
    size_t data_size = shm_size - header_size;
    
    SPDLOG_INFO("SharedMemoryManager: Capacity {}MB, Total size: {} bytes", 
                capacity_mb, shm_size);
}

SharedMemoryManager::SharedMemoryManager(size_t capacity_mb, const std::string& shm_name, bool use_file, const std::string& file_path)
    : capacity_mb(capacity_mb), shm_name(shm_name), shm_fd(-1), shm_base(nullptr),
      shm_size(0), header(nullptr), data_area(nullptr) {
    shm_size = capacity_mb * 1024 * 1024;
    use_file_backing = use_file;
    backing_file_path = file_path;
    SPDLOG_INFO("SharedMemoryManager: Capacity {}MB, Total size: {} bytes", capacity_mb, shm_size);
    if (use_file_backing) {
        SPDLOG_INFO("Using file backing: {}", backing_file_path);
    }
}

SharedMemoryManager::~SharedMemoryManager() {
    cleanup();
}

bool SharedMemoryManager::initialize() {
    try {
        // Create or open shared memory or file backing
        if (use_file_backing) {
            if (!create_file_backing()) {
                SPDLOG_ERROR("Failed to create backing file");
                return false;
            }
        } else {
            if (!create_shared_memory()) {
                SPDLOG_ERROR("Failed to create shared memory");
                return false;
            }
        }
        
        // Map shared memory
        if (!map_shared_memory()) {
            SPDLOG_ERROR("Failed to map shared memory");
            return false;
        }
        
        // Initialize header and data areas
        initialize_header();
        initialize_data_area();
        
        SPDLOG_INFO("SharedMemoryManager initialized successfully");
        SPDLOG_INFO("  Shared memory: {}", shm_name);
        SPDLOG_INFO("  Size: {} MB", capacity_mb);
        SPDLOG_INFO("  Base address: 0x{:x}", header->base_addr);
        SPDLOG_INFO("  Cachelines: {}", header->num_cachelines);
        
        return true;
        
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Exception during initialization: {}", e.what());
        cleanup();
        return false;
    }
}

bool SharedMemoryManager::create_shared_memory() {
    // First, try to open existing shared memory
    shm_fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
    if (shm_fd != -1) {
        // Existing shared memory found - try to use it
        struct stat shm_stat;
        if (fstat(shm_fd, &shm_stat) == 0 && shm_stat.st_size == (off_t)shm_size) {
            SPDLOG_INFO("Reusing existing shared memory: {}", shm_name);
            return true;
        } else {
            // Size mismatch, close and recreate
            close(shm_fd);
            shm_unlink(shm_name.c_str());
        }
    }
    
    // Create new shared memory object
    shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
    if (shm_fd == -1) {
        if (errno == EEXIST) {
            // Race condition - try to unlink and recreate
            shm_unlink(shm_name.c_str());
            shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
            if (shm_fd == -1) {
                SPDLOG_ERROR("Failed to recreate shared memory after unlink: {}", strerror(errno));
                return false;
            }
        } else {
            SPDLOG_ERROR("Failed to create shared memory: {}", strerror(errno));
            return false;
        }
    }
    SPDLOG_INFO("Created new shared memory: {}", shm_name);
    
    // Set size
    if (ftruncate(shm_fd, shm_size) == -1) {
        SPDLOG_ERROR("Failed to set shared memory size: {}", strerror(errno));
        close(shm_fd);
        shm_unlink(shm_name.c_str());
        return false;
    }
    
    return true;
}

bool SharedMemoryManager::create_file_backing() {
    // Create or open regular file for backing
    int fd = open(backing_file_path.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd == -1) {
        SPDLOG_ERROR("Failed to open backing file {}: {}", backing_file_path, strerror(errno));
        return false;
    }
    // Ensure size
    if (ftruncate(fd, shm_size) == -1) {
        SPDLOG_ERROR("Failed to set file size: {}", strerror(errno));
        close(fd);
        return false;
    }
    shm_fd = fd;
    SPDLOG_INFO("Opened backing file: {} ({} bytes)", backing_file_path, shm_size);
    return true;
}

bool SharedMemoryManager::map_shared_memory() {
    // Map the entire shared memory region
    shm_base = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, 
                    MAP_SHARED, shm_fd, 0);
    
    if (shm_base == MAP_FAILED) {
        SPDLOG_ERROR("Failed to map shared memory: {}", strerror(errno));
        return false;
    }
    
    // Set up pointers
    header = reinterpret_cast<SharedMemoryHeader*>(shm_base);
    data_area = reinterpret_cast<uint8_t*>(shm_base) + sizeof(SharedMemoryHeader);
    
    SPDLOG_INFO("Mapped shared memory at address: 0x{:x}", 
                reinterpret_cast<uintptr_t>(shm_base));
    
    return true;
}

void SharedMemoryManager::initialize_header() {
    // Check if already initialized (magic number present)
    if (header->magic == MAGIC_NUMBER && header->version == FORMAT_VERSION) {
        SPDLOG_INFO("Shared memory already initialized, using existing data");
        return;
    }
    
    // Initialize header
    header->magic = MAGIC_NUMBER;
    header->version = FORMAT_VERSION;
    header->total_size = shm_size;
    header->data_offset = sizeof(SharedMemoryHeader);
    header->metadata_offset = 0;  // Metadata is kept locally, not in shared memory
    
    // Support both low addresses (for testing) and high CXL addresses
    // Check environment variable for base address
    const char* base_addr_env = getenv("CXL_BASE_ADDR");
    if (base_addr_env) {
        header->base_addr = strtoull(base_addr_env, NULL, 0);
    } else {
        // Default to 0 to accept any address (will be mapped to shared memory)
        header->base_addr = 0;
    }
    
    // Calculate number of cachelines
    size_t data_area_size = shm_size - sizeof(SharedMemoryHeader);
    header->num_cachelines = data_area_size / CACHELINE_SIZE;
    
    SPDLOG_INFO("Initialized header: {} cachelines available", header->num_cachelines);
    SPDLOG_INFO("Base address: 0x{:x} (0 = accept any address)", header->base_addr);
}

void SharedMemoryManager::initialize_data_area() {
    // Only clear data if this is a fresh initialization (magic number not present)
    size_t data_size = shm_size - sizeof(SharedMemoryHeader);
    if (header->magic != MAGIC_NUMBER) {
        // Clear the data area only for new shared memory
        memset(data_area, 0, data_size);
        SPDLOG_INFO("Cleared data area for new shared memory initialization");
    } else {
        SPDLOG_INFO("Preserving existing data in shared memory");
    }
    
    // Initialize memory regions
    // Start with one large region covering all CXL memory
    MemoryRegion region;
    region.base_addr = header->base_addr;
    region.size = header->num_cachelines * CACHELINE_SIZE;
    region.allocated = false;
    regions.push_back(region);
    
    SPDLOG_INFO("Initialized data area: {} bytes", data_size);
}

void SharedMemoryManager::cleanup() {
    if (shm_base != nullptr && shm_base != MAP_FAILED) {
        munmap(shm_base, shm_size);
        shm_base = nullptr;
    }
    
    if (shm_fd != -1) {
        close(shm_fd);
        shm_fd = -1;
    }
    
    // Note: Not unlinking here so other processes can still access
    // Call shm_unlink(shm_name.c_str()) explicitly if you want to remove
}

SharedMemoryManager::SharedMemoryInfo SharedMemoryManager::get_shm_info() const {
    SharedMemoryInfo info;
    info.shm_name = shm_name;
    info.size = shm_size;
    info.base_addr = header ? header->base_addr : 0;
    info.num_cachelines = header ? header->num_cachelines : 0;
    return info;
}

uint8_t* SharedMemoryManager::get_cacheline_data(uint64_t cacheline_addr) {
    if (!header || !data_area) {
        return nullptr;
    }
    
    // If base_addr is 0, accept any address and use modulo mapping
    if (header->base_addr == 0) {
        uint64_t index = cacheline_to_index(cacheline_addr);
        // cacheline_to_index already handles modulo when base_addr is 0
        return data_area + (index * CACHELINE_SIZE);
    }
    
    // Check if address is valid for non-zero base
    if (cacheline_addr < header->base_addr) {
        return nullptr;
    }
    
    uint64_t index = cacheline_to_index(cacheline_addr);
    if (index >= header->num_cachelines) {
        return nullptr;
    }
    
    // Return pointer to cacheline data in shared memory
    return data_area + (index * CACHELINE_SIZE);
}

bool SharedMemoryManager::read_cacheline(uint64_t addr, uint8_t* buffer, size_t size) {
    // Always allow access when base_addr is 0 (modulo mapping mode)
    if (header && header->base_addr == 0) {
        // Handle reads that might span multiple cachelines
        size_t bytes_read = 0;
        
        while (bytes_read < size) {
            uint64_t current_addr = addr + bytes_read;
            uint64_t cacheline_addr = addr_to_cacheline(current_addr);
            uint64_t index = cacheline_to_index(cacheline_addr);
            uint8_t* cacheline_data = data_area + (index * CACHELINE_SIZE);
            
            size_t offset = current_addr - cacheline_addr;
            size_t bytes_in_cacheline = std::min(size - bytes_read, CACHELINE_SIZE - offset);
            
            memcpy(buffer + bytes_read, cacheline_data + offset, bytes_in_cacheline);
            bytes_read += bytes_in_cacheline;
            
            SPDLOG_DEBUG("Read {} bytes from cacheline at 0x{:x} offset {} (mapped to index {})",
                         bytes_in_cacheline, cacheline_addr, offset, index);
        }
        
        SPDLOG_DEBUG("Total read {} bytes starting at addr 0x{:x}", size, addr);
        return true;
    }
    
    uint64_t cacheline_addr = addr_to_cacheline(addr);
    
    uint8_t* cacheline_data = get_cacheline_data(cacheline_addr);
    if (!cacheline_data) {
        SPDLOG_ERROR("Invalid cacheline address: 0x{:x}", cacheline_addr);
        return false;
    }
    
    // Calculate offset within cacheline
    size_t offset = addr - cacheline_addr;
    if (offset + size > CACHELINE_SIZE) {
        SPDLOG_ERROR("Read crosses cacheline boundary: addr=0x{:x} size={}", addr, size);
        return false;
    }
    
    // Copy data from shared memory
    memcpy(buffer, cacheline_data + offset, size);
    
    SPDLOG_DEBUG("Read {} bytes from addr 0x{:x} (cacheline 0x{:x} offset {})",
                 size, addr, cacheline_addr, offset);
    
    return true;
}

bool SharedMemoryManager::write_cacheline(uint64_t addr, const uint8_t* data, size_t size) {
    // Always allow access when base_addr is 0 (modulo mapping mode)
    if (header && header->base_addr == 0) {
        // Handle writes that might span multiple cachelines
        size_t bytes_written = 0;
        
        while (bytes_written < size) {
            uint64_t current_addr = addr + bytes_written;
            uint64_t cacheline_addr = addr_to_cacheline(current_addr);
            uint64_t index = cacheline_to_index(cacheline_addr);
            uint8_t* cacheline_data = data_area + (index * CACHELINE_SIZE);
            
            size_t offset = current_addr - cacheline_addr;
            size_t bytes_in_cacheline = std::min(size - bytes_written, CACHELINE_SIZE - offset);
            
            memcpy(cacheline_data + offset, data + bytes_written, bytes_in_cacheline);
            bytes_written += bytes_in_cacheline;
            
            SPDLOG_DEBUG("Wrote {} bytes to cacheline at 0x{:x} offset {} (mapped to index {})",
                         bytes_in_cacheline, cacheline_addr, offset, index);
        }
        
        // Use stronger memory barrier for shared memory
        __sync_synchronize();
        // Force sync entire shared memory region to ensure persistence
        if (msync(shm_base, shm_size, MS_SYNC | MS_INVALIDATE) != 0) {
            SPDLOG_ERROR("msync failed: {}", strerror(errno));
            // Critical error - data might not be visible to other processes
            return false;
        }
        
        SPDLOG_DEBUG("Total wrote {} bytes starting at addr 0x{:x}", size, addr);
        return true;
    }
    
    uint64_t cacheline_addr = addr_to_cacheline(addr);
    
    uint8_t* cacheline_data = get_cacheline_data(cacheline_addr);
    if (!cacheline_data) {
        SPDLOG_ERROR("Invalid cacheline address: 0x{:x}", cacheline_addr);
        return false;
    }
    
    // Calculate offset within cacheline
    size_t offset = addr - cacheline_addr;
    if (offset + size > CACHELINE_SIZE) {
        SPDLOG_ERROR("Write crosses cacheline boundary: addr=0x{:x} size={}", addr, size);
        return false;
    }
    
    // Copy data to shared memory
    memcpy(cacheline_data + offset, data, size);
    
    // Memory barrier to ensure write is visible to other processes
    __sync_synchronize();
    // Force sync to backing store
    if (msync(shm_base, shm_size, MS_SYNC | MS_INVALIDATE) != 0) {
        SPDLOG_ERROR("msync failed: {}", strerror(errno));
        // Critical error - data might not be visible to other processes
        return false;
    }
    
    SPDLOG_DEBUG("Wrote {} bytes to addr 0x{:x} (cacheline 0x{:x} offset {})",
                 size, addr, cacheline_addr, offset);
    
    return true;
}

CachelineMetadata* SharedMemoryManager::get_cacheline_metadata(uint64_t cacheline_addr) {
    std::unique_lock<std::shared_mutex> lock(metadata_mutex);
    
    auto it = metadata_cache.find(cacheline_addr);
    if (it != metadata_cache.end()) {
        return it->second.get();
    }
    
    // Create new metadata entry
    auto metadata = std::make_unique<CachelineMetadata>();
    CachelineMetadata* ptr = metadata.get();
    metadata_cache[cacheline_addr] = std::move(metadata);
    
    return ptr;
}

bool SharedMemoryManager::allocate_region(uint64_t addr, size_t size) {
    // Simple allocation tracking
    for (auto& region : regions) {
        if (addr >= region.base_addr && 
            addr + size <= region.base_addr + region.size &&
            !region.allocated) {
            region.allocated = true;
            SPDLOG_INFO("Allocated region: addr=0x{:x} size={}", addr, size);
            return true;
        }
    }
    
    SPDLOG_ERROR("Failed to allocate region: addr=0x{:x} size={}", addr, size);
    return false;
}

bool SharedMemoryManager::deallocate_region(uint64_t addr) {
    for (auto& region : regions) {
        if (region.base_addr == addr && region.allocated) {
            region.allocated = false;
            SPDLOG_INFO("Deallocated region: addr=0x{:x}", addr);
            return true;
        }
    }
    
    return false;
}

bool SharedMemoryManager::is_valid_address(uint64_t addr) const {
    if (!header) {
        return false;
    }
    
    // If base_addr is 0, accept any address (will be mapped with modulo)
    if (header->base_addr == 0) {
        return true;
    }
    
    // Otherwise check if address is in the configured range
    return addr >= header->base_addr && 
           addr < header->base_addr + (header->num_cachelines * CACHELINE_SIZE);
}

SharedMemoryManager::MemoryStats SharedMemoryManager::get_stats() const {
    MemoryStats stats;
    stats.total_capacity = capacity_mb * 1024 * 1024;
    stats.num_cachelines = header ? header->num_cachelines : 0;
    
    // Count active cachelines
    stats.active_cachelines = metadata_cache.size();
    stats.used_memory = stats.active_cachelines * CACHELINE_SIZE;
    
    return stats;
}
