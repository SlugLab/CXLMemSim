/*
 * Shared Memory Communication Interface for CXLMemSim
 * Provides IPC via /dev/shm as an alternative to TCP sockets
 * 
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 */

#ifndef SHM_COMMUNICATION_H
#define SHM_COMMUNICATION_H

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <cstdint>
#include <string>
#include <atomic>
#include <cstring>

// Request/Response structures matching TCP version
struct ShmRequest {
    uint8_t op_type;      // 0=READ, 1=WRITE, 2=GET_SHM_INFO
    uint64_t addr;
    uint64_t size;
    uint64_t timestamp;
    uint8_t data[64];     // Cacheline data
};

struct ShmResponse {
    uint8_t status;
    uint64_t latency_ns;
    uint8_t data[64];
};

// Shared memory ring buffer for communication
struct ShmRingBuffer {
    static constexpr size_t RING_SIZE = 1024;  // Number of entries
    
    // Ring buffer metadata
    std::atomic<uint32_t> head;          // Producer writes here
    std::atomic<uint32_t> tail;          // Consumer reads here
    std::atomic<uint32_t> pending_count; // Number of pending requests
    
    // Padding to avoid false sharing
    uint8_t padding1[64 - sizeof(std::atomic<uint32_t>) * 3];
    
    // Request/Response pairs
    struct Entry {
        std::atomic<bool> request_ready;
        std::atomic<bool> response_ready;
        ShmRequest request;
        ShmResponse response;
        uint8_t padding[64];  // Padding to cacheline boundary
    } entries[RING_SIZE];
    
    // Statistics
    std::atomic<uint64_t> total_requests;
    std::atomic<uint64_t> total_responses;
    
    void initialize() {
        head.store(0);
        tail.store(0);
        pending_count.store(0);
        total_requests.store(0);
        total_responses.store(0);
        
        for (size_t i = 0; i < RING_SIZE; i++) {
            entries[i].request_ready.store(false);
            entries[i].response_ready.store(false);
        }
    }
};

// Client connection info in shared memory
struct ShmClientInfo {
    uint32_t client_id;
    pid_t pid;
    std::atomic<bool> connected;
    char name[256];
    uint8_t padding[64];
};

// Main shared memory communication structure
struct ShmCommunication {
    static constexpr uint64_t MAGIC = 0x434D454D53484D43ULL;  // "CMEMSHMCM"
    static constexpr uint32_t VERSION = 1;
    static constexpr size_t MAX_CLIENTS = 64;
    
    // Header
    uint64_t magic;
    uint32_t version;
    std::atomic<bool> server_ready;
    
    // Semaphores names (stored as strings for cross-process access)
    char request_sem_name[64];   // Signaled when request is ready
    char response_sem_name[64];  // Signaled when response is ready
    
    // Client management
    std::atomic<uint32_t> next_client_id;
    ShmClientInfo clients[MAX_CLIENTS];
    
    // Ring buffers for each client
    ShmRingBuffer ring_buffers[MAX_CLIENTS];
    
    void initialize(const std::string& sem_prefix) {
        magic = MAGIC;
        version = VERSION;
        server_ready.store(false);
        next_client_id.store(0);
        
        // Ensure semaphore names are within POSIX limits (NAME_MAX)
        snprintf(request_sem_name, sizeof(request_sem_name), 
                 "%s_req", sem_prefix.c_str());
        snprintf(response_sem_name, sizeof(response_sem_name), 
                 "%s_resp", sem_prefix.c_str());
        
        for (size_t i = 0; i < MAX_CLIENTS; i++) {
            clients[i].connected.store(false);
            ring_buffers[i].initialize();
        }
    }
    
    bool is_valid() const {
        return magic == MAGIC && version == VERSION;
    }
};

// Shared memory communication manager
class ShmCommunicationManager {
private:
    std::string shm_name;
    int shm_fd;
    ShmCommunication* shm_comm;
    size_t shm_size;
    
    // Semaphores for synchronization
    sem_t* request_sem;
    sem_t* response_sem;
    
    bool is_server;
    uint32_t client_id;  // For client mode
    
public:
    ShmCommunicationManager(const std::string& name, bool server_mode);
    ~ShmCommunicationManager();
    
    // Initialization
    bool initialize();
    void cleanup();
    
    // Server operations
    bool wait_for_request(uint32_t& client_id, ShmRequest& request, int timeout_ms = -1);
    bool send_response(uint32_t client_id, const ShmResponse& response);
    
    // Client operations  
    bool connect(uint32_t& assigned_client_id);
    bool send_request(const ShmRequest& request);
    bool wait_for_response(ShmResponse& response, int timeout_ms = -1);
    void disconnect();
    
    // Helper functions
    bool is_connected() const;
    uint32_t get_client_id() const { return client_id; }
    
    // Statistics
    struct Stats {
        uint64_t total_requests;
        uint64_t total_responses;
        uint32_t active_clients;
    };
    Stats get_stats() const;
    
private:
    bool create_shared_memory();
    bool open_shared_memory();
    bool setup_semaphores();
    void cleanup_semaphores();
    
    // Ring buffer operations
    bool enqueue_request(uint32_t client_id, const ShmRequest& request);
    bool dequeue_request(uint32_t client_id, ShmRequest& request);
    bool enqueue_response(uint32_t client_id, const ShmResponse& response);
    bool dequeue_response(uint32_t client_id, ShmResponse& response);
};

// Helper class for automatic cleanup
class ShmAutoCleanup {
private:
    std::string shm_name;
    std::string sem_prefix;
    
public:
    ShmAutoCleanup(const std::string& name) : shm_name(name) {
        sem_prefix = "/cxlmemsim_" + name;
    }
    
    ~ShmAutoCleanup() {
        // Clean up shared memory
        shm_unlink(shm_name.c_str());
        
        // Clean up semaphores
        std::string req_sem = sem_prefix + "_request";
        std::string resp_sem = sem_prefix + "_response";
        sem_unlink(req_sem.c_str());
        sem_unlink(resp_sem.c_str());
    }
};

#endif // SHM_COMMUNICATION_H