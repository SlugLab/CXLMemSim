/*
 * Shared Memory Communication Implementation for CXLMemSim
 * 
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 */

#include "../include/shm_communication.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <thread>
#include <cerrno>
#include <sys/types.h>

ShmCommunicationManager::ShmCommunicationManager(const std::string& name, bool server_mode)
    : shm_name(name), shm_fd(-1), shm_comm(nullptr), is_server(server_mode), 
      client_id(0), request_sem(nullptr), response_sem(nullptr) {
    shm_size = sizeof(ShmCommunication);
}

ShmCommunicationManager::~ShmCommunicationManager() {
    cleanup();
}

bool ShmCommunicationManager::initialize() {
    if (is_server) {
        if (!create_shared_memory()) {
            SPDLOG_ERROR("Failed to create shared memory");
            return false;
        }
        
        // Initialize the shared memory structure
        // Use shorter semaphore names to avoid POSIX limits
        std::string sem_prefix = "/cxlsim";
        shm_comm->initialize(sem_prefix);
        
        if (!setup_semaphores()) {
            SPDLOG_ERROR("Failed to setup semaphores");
            cleanup();
            return false;
        }
        
        shm_comm->server_ready.store(true);
        SPDLOG_INFO("SHM server initialized: {}", shm_name);
        
    } else {
        if (!open_shared_memory()) {
            SPDLOG_ERROR("Failed to open shared memory");
            return false;
        }
        
        if (!setup_semaphores()) {
            SPDLOG_ERROR("Failed to setup semaphores");
            cleanup();
            return false;
        }
        
        SPDLOG_INFO("SHM client initialized: {}", shm_name);
    }
    
    return true;
}

void ShmCommunicationManager::cleanup() {
    if (shm_comm) {
        if (is_server) {
            shm_comm->server_ready.store(false);
        } else if (client_id > 0 && client_id <= ShmCommunication::MAX_CLIENTS) {
            shm_comm->clients[client_id - 1].connected.store(false);
        }
        
        munmap(shm_comm, shm_size);
        shm_comm = nullptr;
    }
    
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
        
        if (is_server) {
            shm_unlink(shm_name.c_str());
        }
    }
    
    cleanup_semaphores();
}

bool ShmCommunicationManager::create_shared_memory() {
    // Remove any existing shared memory
    shm_unlink(shm_name.c_str());
    
    // Create new shared memory
    shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
    if (shm_fd < 0) {
        SPDLOG_ERROR("Failed to create shared memory '{}': {}", shm_name, strerror(errno));
        return false;
    }
    
    // Set size
    if (ftruncate(shm_fd, shm_size) < 0) {
        SPDLOG_ERROR("Failed to set shared memory size: {}", strerror(errno));
        close(shm_fd);
        shm_fd = -1;
        shm_unlink(shm_name.c_str());
        return false;
    }
    
    // Map shared memory
    shm_comm = static_cast<ShmCommunication*>(
        mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    
    if (shm_comm == MAP_FAILED) {
        SPDLOG_ERROR("Failed to map shared memory: {}", strerror(errno));
        close(shm_fd);
        shm_fd = -1;
        shm_unlink(shm_name.c_str());
        return false;
    }
    
    return true;
}

bool ShmCommunicationManager::open_shared_memory() {
    // Open existing shared memory
    shm_fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
    if (shm_fd < 0) {
        SPDLOG_ERROR("Failed to open shared memory '{}': {}", shm_name, strerror(errno));
        return false;
    }
    
    // Map shared memory
    shm_comm = static_cast<ShmCommunication*>(
        mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    
    if (shm_comm == MAP_FAILED) {
        SPDLOG_ERROR("Failed to map shared memory: {}", strerror(errno));
        close(shm_fd);
        shm_fd = -1;
        return false;
    }
    
    // Validate magic and version
    if (!shm_comm->is_valid()) {
        SPDLOG_ERROR("Invalid shared memory magic or version");
        munmap(shm_comm, shm_size);
        close(shm_fd);
        shm_comm = nullptr;
        shm_fd = -1;
        return false;
    }
    
    // Wait for server to be ready
    int wait_count = 0;
    while (!shm_comm->server_ready.load() && wait_count < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        wait_count++;
    }
    
    if (!shm_comm->server_ready.load()) {
        SPDLOG_ERROR("Server not ready after timeout");
        munmap(shm_comm, shm_size);
        close(shm_fd);
        shm_comm = nullptr;
        shm_fd = -1;
        return false;
    }
    
    return true;
}

bool ShmCommunicationManager::setup_semaphores() {
    if (!shm_comm) return false;
    
    // Clean up any existing semaphores
    cleanup_semaphores();
    
    if (is_server) {
        SPDLOG_DEBUG("Creating semaphores: '{}' and '{}'", 
                    shm_comm->request_sem_name, shm_comm->response_sem_name);
        
        // Create semaphores - unlink first to ensure clean state
        sem_unlink(shm_comm->request_sem_name);
        sem_unlink(shm_comm->response_sem_name);
        
        // Create with O_CREAT only (not O_EXCL) to handle edge cases
        request_sem = sem_open(shm_comm->request_sem_name, O_CREAT, 0666, 0);
        if (request_sem == SEM_FAILED) {
            SPDLOG_ERROR("Failed to create request semaphore '{}': {}", 
                        shm_comm->request_sem_name, strerror(errno));
            return false;
        }
        
        response_sem = sem_open(shm_comm->response_sem_name, O_CREAT, 0666, 0);
        if (response_sem == SEM_FAILED) {
            SPDLOG_ERROR("Failed to create response semaphore '{}': {}", 
                        shm_comm->response_sem_name, strerror(errno));
            sem_close(request_sem);
            sem_unlink(shm_comm->request_sem_name);
            request_sem = nullptr;
            return false;
        }
    } else {
        // Open existing semaphores
        request_sem = sem_open(shm_comm->request_sem_name, 0);
        if (request_sem == SEM_FAILED) {
            SPDLOG_ERROR("Failed to open request semaphore: {}", strerror(errno));
            return false;
        }
        
        response_sem = sem_open(shm_comm->response_sem_name, 0);
        if (response_sem == SEM_FAILED) {
            SPDLOG_ERROR("Failed to open response semaphore: {}", strerror(errno));
            sem_close(request_sem);
            request_sem = nullptr;
            return false;
        }
    }
    
    return true;
}

void ShmCommunicationManager::cleanup_semaphores() {
    if (request_sem) {
        sem_close(request_sem);
        if (is_server && shm_comm) {
            sem_unlink(shm_comm->request_sem_name);
        }
        request_sem = nullptr;
    }
    
    if (response_sem) {
        sem_close(response_sem);
        if (is_server && shm_comm) {
            sem_unlink(shm_comm->response_sem_name);
        }
        response_sem = nullptr;
    }
}

// Server operations
bool ShmCommunicationManager::wait_for_request(uint32_t& out_client_id, ShmRequest& request, int timeout_ms) {
    if (!is_server || !shm_comm) return false;
    
    // Check all client ring buffers for pending requests
    for (uint32_t cid = 1; cid <= ShmCommunication::MAX_CLIENTS; cid++) {
        if (!shm_comm->clients[cid - 1].connected.load()) continue;
        
        auto& ring = shm_comm->ring_buffers[cid - 1];
        uint32_t tail = ring.tail.load(std::memory_order_acquire);
        uint32_t head = ring.head.load(std::memory_order_acquire);
        
        if (tail != head) {
            auto& entry = ring.entries[tail % ShmRingBuffer::RING_SIZE];
            
            if (entry.request_ready.load(std::memory_order_acquire)) {
                // Copy request
                std::memcpy(&request, &entry.request, sizeof(ShmRequest));
                entry.request_ready.store(false, std::memory_order_release);
                
                // Update tail
                ring.tail.store((tail + 1) % ShmRingBuffer::RING_SIZE, std::memory_order_release);
                ring.pending_count.fetch_sub(1, std::memory_order_acq_rel);
                
                out_client_id = cid;
                return true;
            }
        }
    }
    
    // If no requests available, wait on semaphore with timeout
    if (timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        
        if (sem_timedwait(request_sem, &ts) == 0) {
            // Semaphore signaled, retry
            return wait_for_request(out_client_id, request, 0);
        }
    } else if (timeout_ms < 0) {
        // Wait indefinitely
        if (sem_wait(request_sem) == 0) {
            return wait_for_request(out_client_id, request, 0);
        }
    }
    
    return false;
}

bool ShmCommunicationManager::send_response(uint32_t client_id, const ShmResponse& response) {
    if (!is_server || !shm_comm || client_id == 0 || client_id > ShmCommunication::MAX_CLIENTS) {
        return false;
    }
    
    auto& ring = shm_comm->ring_buffers[client_id - 1];
    
    // Find the entry waiting for response
    uint32_t head = ring.head.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < ShmRingBuffer::RING_SIZE; i++) {
        uint32_t idx = (head + i) % ShmRingBuffer::RING_SIZE;
        auto& entry = ring.entries[idx];
        
        if (!entry.request_ready.load() && !entry.response_ready.load()) {
            // Copy response
            std::memcpy(&entry.response, &response, sizeof(ShmResponse));
            entry.response_ready.store(true, std::memory_order_release);
            
            // Signal response semaphore
            sem_post(response_sem);
            
            ring.total_responses.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    
    return false;
}

// Client operations
bool ShmCommunicationManager::connect(uint32_t& assigned_client_id) {
    if (is_server || !shm_comm) return false;
    
    // Find an available client slot
    for (uint32_t i = 0; i < ShmCommunication::MAX_CLIENTS; i++) {
        bool expected = false;
        if (shm_comm->clients[i].connected.compare_exchange_strong(expected, true)) {
            client_id = i + 1;
            assigned_client_id = client_id;
            
            // Fill client info
            shm_comm->clients[i].client_id = client_id;
            shm_comm->clients[i].pid = getpid();
            snprintf(shm_comm->clients[i].name, sizeof(shm_comm->clients[i].name),
                     "Client_%d", client_id);
            
            // Initialize ring buffer for this client
            shm_comm->ring_buffers[i].initialize();
            
            SPDLOG_INFO("Connected as client {}", client_id);
            return true;
        }
    }
    
    SPDLOG_ERROR("No available client slots");
    return false;
}

bool ShmCommunicationManager::send_request(const ShmRequest& request) {
    if (is_server || !shm_comm || client_id == 0) return false;
    
    auto& ring = shm_comm->ring_buffers[client_id - 1];
    
    // Check if ring buffer is full
    uint32_t head = ring.head.load(std::memory_order_acquire);
    uint32_t next_head = (head + 1) % ShmRingBuffer::RING_SIZE;
    uint32_t tail = ring.tail.load(std::memory_order_acquire);
    
    if (next_head == tail) {
        SPDLOG_WARN("Ring buffer full for client {}", client_id);
        return false;
    }
    
    auto& entry = ring.entries[head];
    
    // Copy request
    std::memcpy(&entry.request, &request, sizeof(ShmRequest));
    entry.request_ready.store(true, std::memory_order_release);
    entry.response_ready.store(false, std::memory_order_release);
    
    // Update head
    ring.head.store(next_head, std::memory_order_release);
    ring.pending_count.fetch_add(1, std::memory_order_acq_rel);
    ring.total_requests.fetch_add(1, std::memory_order_relaxed);
    
    // Signal request semaphore
    sem_post(request_sem);
    
    return true;
}

bool ShmCommunicationManager::wait_for_response(ShmResponse& response, int timeout_ms) {
    if (is_server || !shm_comm || client_id == 0) return false;
    
    auto& ring = shm_comm->ring_buffers[client_id - 1];
    
    // Wait for response with timeout
    auto start = std::chrono::steady_clock::now();
    
    while (true) {
        // Check for response
        for (uint32_t i = 0; i < ShmRingBuffer::RING_SIZE; i++) {
            auto& entry = ring.entries[i];
            
            if (entry.response_ready.load(std::memory_order_acquire)) {
                // Copy response
                std::memcpy(&response, &entry.response, sizeof(ShmResponse));
                entry.response_ready.store(false, std::memory_order_release);
                return true;
            }
        }
        
        // Check timeout
        if (timeout_ms >= 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeout_ms) {
                return false;
            }
        }
        
        // Wait on semaphore with short timeout
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10000000;  // 10ms
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        
        sem_timedwait(response_sem, &ts);
    }
    
    return false;
}

void ShmCommunicationManager::disconnect() {
    if (!is_server && shm_comm && client_id > 0 && client_id <= ShmCommunication::MAX_CLIENTS) {
        shm_comm->clients[client_id - 1].connected.store(false);
        client_id = 0;
    }
}

bool ShmCommunicationManager::is_connected() const {
    if (is_server) {
        return shm_comm && shm_comm->server_ready.load();
    } else {
        return shm_comm && client_id > 0 && 
               client_id <= ShmCommunication::MAX_CLIENTS &&
               shm_comm->clients[client_id - 1].connected.load();
    }
}

ShmCommunicationManager::Stats ShmCommunicationManager::get_stats() const {
    Stats stats = {0, 0, 0};
    
    if (!shm_comm) return stats;
    
    for (uint32_t i = 0; i < ShmCommunication::MAX_CLIENTS; i++) {
        if (shm_comm->clients[i].connected.load()) {
            stats.active_clients++;
        }
        stats.total_requests += shm_comm->ring_buffers[i].total_requests.load();
        stats.total_responses += shm_comm->ring_buffers[i].total_responses.load();
    }
    
    return stats;
}