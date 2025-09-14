/*
 * CXLMemSim controller - Thread-per-Connection Server Mode
 * Multi-threaded server with one thread per client connection, shared CXLController,
 * coherency protocol, and congestion handling
 *
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "cxlcontroller.h"
#include "cxlendpoint.h"
#include "helper.h"
#include "policy.h"
#include "shared_memory_manager.h"
#include "../include/shm_communication.h"
#include <cerrno>
#include <cxxopts.hpp>
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <queue>
#include <map>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <errno.h>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <set>
#include <fstream>
#include <iterator>
#include <queue>

#ifndef MSG_WAITALL
#define MSG_WAITALL 0x100
#endif

// Global variables
Helper helper{};
CXLController* controller = nullptr;

// Server request/response structures (matching qemu_integration)
struct ServerRequest {
    uint8_t op_type;      // 0=READ, 1=WRITE, 2=GET_SHM_INFO
    uint64_t addr;
    uint64_t size;
    uint64_t timestamp;
    uint8_t data[64];     // Cacheline data
};

struct ServerResponse {
    uint8_t status;
    uint64_t latency_ns;
    uint8_t data[64];
};

// Extended response for shared memory info
struct SharedMemoryInfoResponse {
    uint8_t status;
    uint64_t base_addr;
    uint64_t size;
    uint64_t num_cachelines;
    char shm_name[256];
};

// Using CachelineMetadata from SharedMemoryManager
// CoherencyState is already defined in shared_memory_manager.h
using CachelineInfo = CachelineMetadata;

// Back invalidation callback entry
struct BackInvalidationEntry {
    uint64_t cacheline_addr;
    int source_thread_id;
    uint64_t timestamp;
    std::vector<uint8_t> dirty_data;
};

// Communication mode enum
enum class CommMode {
    TCP,
    SHM  // Shared Memory via /dev/shm
};

// Thread-per-connection server class
class ThreadPerConnectionServer {
private:
    int server_fd;
    int port;
    CXLController* controller;
    std::atomic<bool> running;
    std::atomic<int> next_thread_id;
    
    // Communication mode
    CommMode comm_mode;
    std::unique_ptr<ShmCommunicationManager> shm_comm_manager;
    
    // Shared memory manager for real memory allocation
    std::unique_ptr<SharedMemoryManager> shm_manager;
    std::string backing_file_;
    
    // Memory storage with coherency tracking (metadata only)
    // Actual data is in shared memory managed by shm_manager
    std::shared_mutex memory_mutex;
    
    // Congestion tracking
    struct CongestionInfo {
        std::atomic<int> active_requests;
        std::atomic<uint64_t> total_bandwidth_used;
        std::chrono::steady_clock::time_point last_reset;
        std::mutex reset_mutex;
    };
    CongestionInfo congestion_info;
    
    // Thread management
    std::vector<std::thread> client_threads;
    std::mutex thread_list_mutex;
    
    // Back invalidation tracking
    std::map<uint64_t, std::queue<BackInvalidationEntry>> back_invalidation_queue;
    std::shared_mutex back_invalidation_mutex;
    
    // Statistics
    std::atomic<uint64_t> total_reads{0};
    std::atomic<uint64_t> total_writes{0};
    std::atomic<uint64_t> coherency_invalidations{0};
    std::atomic<uint64_t> coherency_downgrades{0};
    std::atomic<uint64_t> back_invalidations{0};
    
public:
    ThreadPerConnectionServer(int port, CXLController* ctrl, size_t capacity_mb, 
                            const std::string& backing_file = "", CommMode mode = CommMode::TCP)
        : port(port), controller(ctrl), running(true), next_thread_id(0), 
          backing_file_(backing_file), comm_mode(mode) {
        congestion_info.active_requests = 0;
        congestion_info.total_bandwidth_used = 0;
        congestion_info.last_reset = std::chrono::steady_clock::now();
        
        // Initialize shared memory manager
        if (!backing_file_.empty()) {
            SPDLOG_INFO("Using backing file for memory: {}", backing_file_);
            shm_manager = std::make_unique<SharedMemoryManager>(capacity_mb, "/cxlmemsim_shared", true, backing_file_);
        } else {
            shm_manager = std::make_unique<SharedMemoryManager>(capacity_mb);
        }
    }
    
    bool start();
    void run();
    void stop();
    void handle_client(int client_fd, int thread_id);
    
    // Shared memory mode methods
    void run_shm_mode();
    void handle_shm_requests();
    
private:
    // Coherency protocol methods
    void handle_read_coherency(uint64_t cacheline_addr, int thread_id, CachelineInfo& info);
    void handle_write_coherency(uint64_t cacheline_addr, int thread_id, CachelineInfo& info);
    void invalidate_sharers(uint64_t cacheline_addr, int requesting_thread, CachelineInfo& info);
    void downgrade_owner(uint64_t cacheline_addr, int requesting_thread, CachelineInfo& info);
    
    // Congestion handling
    double calculate_congestion_factor();
    void update_congestion_stats(uint64_t bytes_transferred);
    
    // Request handling
    void handle_request(int client_fd, int thread_id, ServerRequest& req, ServerResponse& resp);
    uint64_t calculate_total_latency(uint64_t base_latency, double congestion_factor, 
                                   bool had_coherency_miss, uint64_t size);
    
    // Back invalidation methods
    void register_back_invalidation(uint64_t cacheline_addr, int source_thread_id, 
                                  const std::vector<uint8_t>& dirty_data, uint64_t timestamp);
    bool check_and_apply_back_invalidations(uint64_t cacheline_addr, int requesting_thread_id, 
                                           CachelineInfo& info);
};

static ThreadPerConnectionServer* g_server = nullptr;

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    if (g_server) {
        SPDLOG_INFO("Shutting down server...");
        g_server->stop();
    }
    exit(0);
}

int main(int argc, char *argv[]) {
    spdlog::cfg::load_env_levels();
    cxxopts::Options options("CXLMemSim Server", "CXL.mem Type 3 Memory Controller Thread-per-Connection Server");
    
    options.add_options()
        ("h,help", "Help for CXLMemSim Server", cxxopts::value<bool>()->default_value("false"))
        ("v,verbose", "Verbose level", cxxopts::value<int>()->default_value("2"))
        ("default_latency", "Default latency", cxxopts::value<size_t>()->default_value("100"))
        ("interleave_size", "Interleave size", cxxopts::value<size_t>()->default_value("256"))
        ("capacity", "Capacity of CXL expander in MB", cxxopts::value<int>()->default_value("256"))
        ("p,port", "Server port", cxxopts::value<int>()->default_value("9999"))
        ("t,topology", "Topology file", cxxopts::value<std::string>()->default_value("topology.txt"))
        ("backing-file", "Back CXL memory with a regular file (shared across VMs)", cxxopts::value<std::string>()->default_value(""))
        ("comm-mode", "Communication mode: tcp or shm (shared memory)", cxxopts::value<std::string>()->default_value("tcp"));

    auto result = options.parse(argc, argv);
    
    std::string backing_file = result["backing-file"].as<std::string>();
    if (result.count("help")) {
        fmt::print("{}\n", options.help());
        return 0;
    }

    int verbose = result["verbose"].as<int>();
    size_t default_latency = result["default_latency"].as<size_t>();
    size_t interleave_size = result["interleave_size"].as<size_t>();
    int capacity = result["capacity"].as<int>();
    int port = result["port"].as<int>();
    std::string topology = result["topology"].as<std::string>();
    std::string comm_mode_str = result["comm-mode"].as<std::string>();
    
    // Parse communication mode
    CommMode comm_mode = CommMode::TCP;
    if (comm_mode_str == "shm" || comm_mode_str == "shared_memory") {
        comm_mode = CommMode::SHM;
    } else if (comm_mode_str != "tcp") {
        SPDLOG_ERROR("Invalid communication mode: {}. Use 'tcp' or 'shm'", comm_mode_str);
        return 1;
    }
    
    // Initialize policies
    std::array<Policy *, 4> policies = {
        new AllocationPolicy(),
        new MigrationPolicy(),
        new PagingPolicy(),
        new CachingPolicy()
    };

    // Create controller
    controller = new CXLController(policies, capacity, PAGE, 10, default_latency);
    
    // Load topology if file exists
    if (access(topology.c_str(), F_OK) == 0) {
        SPDLOG_INFO("Loading topology from {}", topology);
        // Read topology file
        std::ifstream topo_file(topology);
        if (topo_file.is_open()) {
            std::string topo_content((std::istreambuf_iterator<char>(topo_file)),
                                   std::istreambuf_iterator<char>());
            controller->construct_topo(topo_content);
            topo_file.close();
        }
    } else {
        SPDLOG_WARN("Topology file {} not found, using default configuration", topology);
    }
    
    SPDLOG_INFO("========================================");
    SPDLOG_INFO("CXLMemSim CXL Type3 Memory Server");
    SPDLOG_INFO("========================================");
    SPDLOG_INFO("Server Configuration:");
    SPDLOG_INFO("  Communication Mode: {}", comm_mode == CommMode::TCP ? "TCP" : "Shared Memory (/dev/shm)");
    SPDLOG_INFO("  Port: {}", port);
    SPDLOG_INFO("  Topology: {}", topology);
    SPDLOG_INFO("  Capacity: {} MB", capacity);
    SPDLOG_INFO("  Default latency: {} ns", default_latency);
    SPDLOG_INFO("  Interleave size: {} bytes", interleave_size);
    SPDLOG_INFO("CXL Type3 Operations Supported:");
    SPDLOG_INFO("  - CXL_TYPE3_READ");
    SPDLOG_INFO("  - CXL_TYPE3_WRITE");
    SPDLOG_INFO("========================================");
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    try {
        ThreadPerConnectionServer server(port, controller, capacity, backing_file, comm_mode);
        g_server = &server;
        
        if (!server.start()) {
            SPDLOG_ERROR("Failed to start server");
            return 1;
        }
        
        server.run();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Server error: {}", e.what());
        return 1;
    }
    
    return 0;
}

// ThreadPerConnectionServer implementation
bool ThreadPerConnectionServer::start() {
    // Initialize shared memory first
    if (!shm_manager->initialize()) {
        SPDLOG_ERROR("Failed to initialize shared memory");
        return false;
    }
    
    auto shm_info = shm_manager->get_shm_info();
    SPDLOG_INFO("Shared memory initialized:");
    SPDLOG_INFO("  Name: {}", shm_info.shm_name);
    SPDLOG_INFO("  Size: {} bytes", shm_info.size);
    SPDLOG_INFO("  Base address: 0x{:x}", shm_info.base_addr);
    SPDLOG_INFO("  Cachelines: {}", shm_info.num_cachelines);
    
    if (comm_mode == CommMode::SHM) {
        // Initialize shared memory communication
        shm_comm_manager = std::make_unique<ShmCommunicationManager>("/cxlmemsim_comm", true);
        if (!shm_comm_manager->initialize()) {
            SPDLOG_ERROR("Failed to initialize shared memory communication");
            return false;
        }
        SPDLOG_INFO("Server using shared memory communication mode");
        return true;
    }
    
    // TCP mode initialization
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        SPDLOG_ERROR("Failed to create socket");
        return false;
    }
    
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        SPDLOG_ERROR("Failed to set socket options");
        return false;
    }
    
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        SPDLOG_ERROR("Failed to bind to port {}", port);
        return false;
    }
    
    if (listen(server_fd, 100) < 0) {
        SPDLOG_ERROR("Failed to listen on socket");
        return false;
    }
    
    SPDLOG_INFO("Server listening on port {}", port);
    return true;
}

void ThreadPerConnectionServer::run() {
    if (comm_mode == CommMode::SHM) {
        run_shm_mode();
        return;
    }
    
    // TCP mode
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (running) {
                SPDLOG_ERROR("Failed to accept connection");
            }
            continue;
        }
        
        // Create a new thread for this client
        int thread_id = next_thread_id++;
        
        {
            std::lock_guard<std::mutex> lock(thread_list_mutex);
            // Pass thread_id to handle_client
            client_threads.emplace_back(&ThreadPerConnectionServer::handle_client, this, client_fd, thread_id);
            client_threads.back().detach();  // Detach to allow independent execution
        }
        
        SPDLOG_INFO("Accepted new client connection, assigned thread ID {}", thread_id);
    }
}

void ThreadPerConnectionServer::stop() {
    running = false;
    close(server_fd);
    
    // Print final statistics
    SPDLOG_INFO("Server Statistics:");
    SPDLOG_INFO("  Total Reads: {}", total_reads.load());
    SPDLOG_INFO("  Total Writes: {}", total_writes.load());
    SPDLOG_INFO("  Coherency Invalidations: {}", coherency_invalidations.load());
    SPDLOG_INFO("  Coherency Downgrades: {}", coherency_downgrades.load());
    SPDLOG_INFO("  Back Invalidations: {}", back_invalidations.load());
}

void ThreadPerConnectionServer::handle_read_coherency(uint64_t cacheline_addr, int thread_id, CachelineInfo& info) {
    switch (info.state) {
        case INVALID:
            info.state = SHARED;
            info.sharers.insert(thread_id);
            break;
            
        case SHARED:
            info.sharers.insert(thread_id);
            break;
            
        case EXCLUSIVE:
        case MODIFIED:
            // Downgrade to SHARED
            downgrade_owner(cacheline_addr, thread_id, info);
            info.state = SHARED;
            info.sharers.insert(info.owner);
            info.sharers.insert(thread_id);
            info.owner = -1;
            break;
    }
}

void ThreadPerConnectionServer::handle_write_coherency(uint64_t cacheline_addr, int thread_id, CachelineInfo& info) {
    // Check if we need to register back invalidation for sharers
    bool need_back_invalidation = false;
    std::set<int> sharers_to_invalidate;
    
    switch (info.state) {
        case INVALID:
            info.state = MODIFIED;
            info.owner = thread_id;
            break;
            
        case SHARED:
            // Need to invalidate all sharers
            sharers_to_invalidate = info.sharers;
            need_back_invalidation = true;
            invalidate_sharers(cacheline_addr, thread_id, info);
            info.state = MODIFIED;
            info.owner = thread_id;
            info.sharers.clear();
            break;
            
        case EXCLUSIVE:
        case MODIFIED:
            if (info.owner != thread_id) {
                // Need to invalidate current owner
                sharers_to_invalidate.insert(info.owner);
                need_back_invalidation = true;
                invalidate_sharers(cacheline_addr, thread_id, info);
            }
            info.state = MODIFIED;
            info.owner = thread_id;
            break;
    }
    
    // Mark that this cacheline has a dirty update
    if (need_back_invalidation) {
        info.has_dirty_update = true;
        info.dirty_update_time = std::chrono::steady_clock::now().time_since_epoch().count();
    }
}

void ThreadPerConnectionServer::invalidate_sharers(uint64_t cacheline_addr, int requesting_thread, CachelineInfo& info) {
    for (int sharer : info.sharers) {
        if (sharer != requesting_thread) {
            coherency_invalidations++;
            SPDLOG_DEBUG("Invalidating cacheline 0x{:x} in thread {}", cacheline_addr, sharer);
        }
    }
    if (info.owner != -1 && info.owner != requesting_thread) {
        coherency_invalidations++;
        SPDLOG_DEBUG("Invalidating cacheline 0x{:x} in owner thread {}", cacheline_addr, info.owner);
    }
}

void ThreadPerConnectionServer::downgrade_owner(uint64_t cacheline_addr, int requesting_thread, CachelineInfo& info) {
    if (info.owner != -1 && info.owner != requesting_thread) {
        coherency_downgrades++;
        SPDLOG_DEBUG("Downgrading cacheline 0x{:x} from thread {}", cacheline_addr, info.owner);
    }
}

double ThreadPerConnectionServer::calculate_congestion_factor() {
    std::lock_guard<std::mutex> lock(congestion_info.reset_mutex);
    
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - congestion_info.last_reset);
    
    // Reset stats every second
    if (duration.count() > 1000) {
        congestion_info.total_bandwidth_used = 0;
        congestion_info.last_reset = now;
    }
    
    // Calculate congestion based on active requests and bandwidth usage
    int active = congestion_info.active_requests.load();
    uint64_t bandwidth_used = congestion_info.total_bandwidth_used.load();
    
    // Simple congestion model: factor increases with active requests
    // Base factor is 1.0, increases by 0.1 for every 10 active requests
    double congestion_factor = 1.0 + (active / 10.0) * 0.1;
    
    // Additional factor based on bandwidth usage (assuming 64GB/s max)
    if (bandwidth_used > 64ULL * 1024 * 1024 * 1024) {  // Over 64GB/s
        congestion_factor *= 1.5;
    }
    
    return congestion_factor;
}

void ThreadPerConnectionServer::update_congestion_stats(uint64_t bytes_transferred) {
    congestion_info.total_bandwidth_used += bytes_transferred;
}

uint64_t ThreadPerConnectionServer::calculate_total_latency(uint64_t base_latency, double congestion_factor, 
                                                           bool had_coherency_miss, uint64_t size) {
    double latency = base_latency;
    
    // Apply congestion factor
    latency *= congestion_factor;
    
    // Add coherency miss penalty (50ns for invalidation/downgrade)
    if (had_coherency_miss) {
        latency += 50;
    }
    
    // Add transfer time based on bandwidth (64GB/s)
    double transfer_time_ns = (size * 8.0) / (64.0 * 1e9) * 1e9;
    latency += transfer_time_ns;
    
    return static_cast<uint64_t>(latency);
}

void ThreadPerConnectionServer::handle_request(int client_fd, int thread_id, ServerRequest& req, ServerResponse& resp) {
    uint64_t cacheline_addr = req.addr & ~(63ULL);  // 64-byte aligned
    bool had_coherency_miss = false;
    
    // Log CXL Type3 operation with detailed information
    const char* op_name = (req.op_type == 0) ? "CXL_TYPE3_READ" : "CXL_TYPE3_WRITE";
    
    // Log incoming request details
    // SPDLOG_INFO("Thread {}: {} request - addr=0x{:x}, size={}, cacheline=0x{:x}", 
    //             thread_id, op_name, req.addr, req.size, cacheline_addr);
    
    if (req.op_type == 1) {  // WRITE
        // Log first 16 bytes of write data
        std::stringstream data_str;
        for (int i = 0; i < std::min(16UL, req.size); i++) {
            data_str << std::hex << std::setfill('0') << std::setw(2) 
                    << static_cast<int>(req.data[i]) << " ";
        }
        // SPDLOG_INFO("Thread {}: WRITE data (first 16 bytes): {}", 
        //            thread_id, data_str.str());
    }
    
    // Check if address is valid in shared memory
    // if (!shm_manager->is_valid_address(req.addr)) {
    //     SPDLOG_ERROR("Thread {}: Invalid address 0x{:x} not in CXL memory range", 
    //                 thread_id, req.addr);
    //     resp.status = 1;
    //     return;
    // }
    
    // Increment active requests
    congestion_info.active_requests++;
    
    // Calculate base latency using CXL controller
    std::vector<std::tuple<uint64_t, uint64_t>> access_elem;
    access_elem.push_back(std::make_tuple(req.addr, req.size));
    double base_latency = controller->calculate_latency(access_elem, controller->dramlatency);
    
    // Handle coherency and memory operation
    {
        std::unique_lock<std::shared_mutex> lock(memory_mutex);
        
        // Get metadata from shared memory manager
        auto* metadata = shm_manager->get_cacheline_metadata(cacheline_addr);
        if (!metadata) {
            SPDLOG_ERROR("Thread {}: Failed to get metadata for cacheline 0x{:x}", 
                        thread_id, cacheline_addr);
            resp.status = 1;
            congestion_info.active_requests--;
            return;
        }
        
        // Lock the cacheline for this operation
        std::lock_guard<std::mutex> cacheline_lock(metadata->lock);
        
        // Reference to metadata for compatibility with existing code
        auto& info = *metadata;
        
        // Check if we need coherency actions
        if (req.op_type == 0) {  // READ
            // SPDLOG_DEBUG("Thread {}: CXL_TYPE3_READ processing - checking coherency for cacheline 0x{:x}", 
            //             thread_id, cacheline_addr);
            
            // First check for back invalidations
            bool had_back_invalidation = check_and_apply_back_invalidations(cacheline_addr, thread_id, info);
            
            if (info.state == EXCLUSIVE || info.state == MODIFIED) {
                if (info.owner != -1 && info.owner != thread_id) {
                    had_coherency_miss = true;
                    SPDLOG_DEBUG("Thread {}: CXL_TYPE3_READ coherency miss - cacheline owned by thread {}", 
                                thread_id, info.owner);
                }
            }
            handle_read_coherency(cacheline_addr, thread_id, info);
            
            // Read data from shared memory
            if (!shm_manager->read_cacheline(req.addr, resp.data, req.size)) {
                SPDLOG_ERROR("Thread {}: Failed to read from shared memory at 0x{:x}", 
                            thread_id, req.addr);
                resp.status = 1;
                congestion_info.active_requests--;
                return;
            }
            
            // Log the data being read
            std::stringstream read_data_str;
            for (int i = 0; i < std::min(16UL, req.size); i++) {
                read_data_str << std::hex << std::setfill('0') << std::setw(2) 
                             << static_cast<int>(resp.data[i]) << " ";
            }
            // SPDLOG_INFO("Thread {}: READ response data (first 16 bytes): {}", 
            //            thread_id, read_data_str.str());
            
            // Add back invalidation latency penalty if we had one
            if (had_back_invalidation) {
                had_coherency_miss = true;  // Treat back invalidation as coherency miss
                SPDLOG_DEBUG("Thread {}: CXL_TYPE3_READ had back invalidation for cacheline 0x{:x}", 
                            thread_id, cacheline_addr);
            }
            
            total_reads++;
        } else {  // WRITE
            // SPDLOG_DEBUG("Thread {}: CXL_TYPE3_WRITE processing - checking coherency for cacheline 0x{:x}", 
            //             thread_id, cacheline_addr);
            
            if (info.state == SHARED && !info.sharers.empty()) {
                had_coherency_miss = true;
                SPDLOG_DEBUG("Thread {}: CXL_TYPE3_WRITE coherency miss - cacheline shared by {} threads", 
                            thread_id, info.sharers.size());
            } else if ((info.state == EXCLUSIVE || info.state == MODIFIED) && 
                      info.owner != thread_id) {
                had_coherency_miss = true;
                SPDLOG_DEBUG("Thread {}: CXL_TYPE3_WRITE coherency miss - cacheline owned by thread {}", 
                            thread_id, info.owner);
            }
            // Keep track of who needs invalidation before state change
            std::set<int> threads_to_invalidate;
            if (info.state == SHARED) {
                threads_to_invalidate = info.sharers;
            } else if ((info.state == EXCLUSIVE || info.state == MODIFIED) && info.owner != thread_id) {
                threads_to_invalidate.insert(info.owner);
            }
            
            if (!threads_to_invalidate.empty()) {
                // SPDLOG_INFO("Thread {}: CXL_TYPE3_WRITE invalidating {} threads for cacheline 0x{:x}", 
                        //    thread_id, threads_to_invalidate.size(), cacheline_addr);
            }
            
            handle_write_coherency(cacheline_addr, thread_id, info);
            
            // Write data to shared memory
            if (!shm_manager->write_cacheline(req.addr, req.data, req.size)) {
                SPDLOG_ERROR("Thread {}: Failed to write to shared memory at 0x{:x}", 
                            thread_id, req.addr);
                resp.status = 1;
                congestion_info.active_requests--;
                return;
            }
            
            // SPDLOG_INFO("Thread {}: WRITE completed successfully at addr=0x{:x}, size={}", 
                    //    thread_id, req.addr, req.size);
            
            // Verify write by reading back
            uint8_t verify_data[64];
            if (shm_manager->read_cacheline(req.addr, verify_data, req.size)) {
                std::stringstream verify_str;
                for (int i = 0; i < std::min(16UL, req.size); i++) {
                    verify_str << std::hex << std::setfill('0') << std::setw(2) 
                              << static_cast<int>(verify_data[i]) << " ";
                }
                SPDLOG_INFO("Thread {}: WRITE verification - data in memory: {}", 
                           thread_id, verify_str.str());
            }
            
            // Register back invalidation for threads that had this cacheline
            if (!threads_to_invalidate.empty()) {
                // Read the dirty data from shared memory for back invalidation
                std::vector<uint8_t> dirty_data(req.size);
                shm_manager->read_cacheline(req.addr, dirty_data.data(), req.size);
                
                for (int invalidated_thread : threads_to_invalidate) {
                    if (invalidated_thread != thread_id) {
                        register_back_invalidation(cacheline_addr, thread_id, dirty_data, req.timestamp);
                    }
                }
            }
            
            total_writes++;
        }
        
        info.last_access_time = req.timestamp;
    }
    
    // Calculate congestion factor
    double congestion_factor = calculate_congestion_factor();
    
    // Update congestion statistics
    update_congestion_stats(req.size);
    
    // Calculate total latency including congestion and coherency effects
    uint64_t total_latency = calculate_total_latency(base_latency, congestion_factor, 
                                                    had_coherency_miss, req.size);
    
    // Decrement active requests
    congestion_info.active_requests--;
    
    // Fill response
    resp.status = 0;
    resp.latency_ns = total_latency;
    
    // Enhanced logging for CXL Type3 operations
    const char* op_result = (req.op_type == 0) ? "CXL_TYPE3_READ_COMPLETE" : "CXL_TYPE3_WRITE_COMPLETE";
    // SPDLOG_INFO("Thread {}: {} addr=0x{:x} size={} latency={}ns (base={}ns congestion={:.2f}x coherency_miss={})",
    //             thread_id, op_result, req.addr, req.size, 
                // total_latency, static_cast<uint64_t>(base_latency), congestion_factor, had_coherency_miss);
    
    // Log cache state changes
    // if (had_coherency_miss) {
    //     SPDLOG_INFO("Thread {}: Coherency action for addr=0x{:x} cacheline=0x{:x}", 
    //                 thread_id, req.addr, cacheline_addr);
    // }
    
    // Detailed debug logging
    // SPDLOG_DEBUG("Thread {}: Memory access details - cacheline_addr=0x{:x} offset={} data_size={}",
    //             thread_id, cacheline_addr, req.addr - cacheline_addr, req.size);
}

void ThreadPerConnectionServer::handle_client(int client_fd, int thread_id) {
    // Thread ID is now passed as parameter
    
    // Get client information for debugging
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    if (getpeername(client_fd, (struct sockaddr*)&client_addr, &addr_len) == 0) {
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        SPDLOG_INFO("Thread {}: Client connected from {}:{} (fd={})", 
                   thread_id, client_ip, ntohs(client_addr.sin_port), client_fd);
    } else {
        SPDLOG_INFO("Thread {}: Client connected (fd={})", thread_id, client_fd);
    }
    
    while (running) {
        ServerRequest req;
        ssize_t received = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
        
        if (received != sizeof(req)) {
            if (received == 0) {
                // This might be a probe connection - don't log as error
                SPDLOG_DEBUG("Thread {}: Client disconnected (probe connection?)", thread_id);
            } else if (received < 0) {
                int err = errno;
                if (err == ECONNRESET) {
                    SPDLOG_INFO("Thread {}: Connection reset by peer", thread_id);
                } else if (err == ETIMEDOUT) {
                    SPDLOG_INFO("Thread {}: Connection timed out", thread_id);
                } else if (err == EAGAIN || err == EWOULDBLOCK) {
                    // Non-blocking socket, no data available
                    continue;
                } else {
                    SPDLOG_ERROR("Thread {}: recv() failed with error: {} ({})", 
                                thread_id, strerror(err), err);
                }
            } else {
                SPDLOG_ERROR("Thread {}: Incomplete request - received {} bytes, expected {}", 
                            thread_id, received, sizeof(req));
                // Dump what we received for debugging
                if (received > 0) {
                    std::stringstream hex_dump;
                    unsigned char* buf = (unsigned char*)&req;
                    for (ssize_t i = 0; i < received && i < 32; i++) {
                        hex_dump << std::hex << std::setfill('0') << std::setw(2) 
                                << (int)buf[i] << " ";
                    }
                    SPDLOG_DEBUG("Thread {}: Partial data: {}", thread_id, hex_dump.str());
                }
            }
            break;
        }
        
        // Handle special request for shared memory info
        if (req.op_type == 2) {  // GET_SHM_INFO
            SharedMemoryInfoResponse shm_resp = {0};
            auto shm_info = shm_manager->get_shm_info();
            
            shm_resp.status = 0;
            shm_resp.base_addr = shm_info.base_addr;
            shm_resp.size = shm_info.size;
            shm_resp.num_cachelines = shm_info.num_cachelines;
            strncpy(shm_resp.shm_name, shm_info.shm_name.c_str(), sizeof(shm_resp.shm_name) - 1);
            
            SPDLOG_INFO("Thread {}: Sending shared memory info - name: {}, size: {} bytes", 
                       thread_id, shm_info.shm_name, shm_info.size);
            
            ssize_t sent = send(client_fd, &shm_resp, sizeof(shm_resp), 0);
            if (sent != sizeof(shm_resp)) {
                SPDLOG_ERROR("Thread {}: Failed to send shared memory info", thread_id);
                break;
            }
            continue;
        }
        
        // Regular memory operation
        ServerResponse resp = {0};
        handle_request(client_fd, thread_id, req, resp);
        
        ssize_t sent = send(client_fd, &resp, sizeof(resp), 0);
        if (sent != sizeof(resp)) {
            SPDLOG_ERROR("Thread {}: Failed to send response", thread_id);
            break;
        }
    }
    
    close(client_fd);
    SPDLOG_INFO("Thread {}: Connection closed", thread_id);
}

// Back invalidation implementation
void ThreadPerConnectionServer::register_back_invalidation(uint64_t cacheline_addr, int source_thread_id,
                                                         const std::vector<uint8_t>& dirty_data, uint64_t timestamp) {
    std::unique_lock<std::shared_mutex> lock(back_invalidation_mutex);
    
    BackInvalidationEntry entry;
    entry.cacheline_addr = cacheline_addr;
    entry.source_thread_id = source_thread_id;
    entry.timestamp = timestamp;
    entry.dirty_data = dirty_data;
    
    back_invalidation_queue[cacheline_addr].push(entry);
    
    SPDLOG_DEBUG("Registered back invalidation for cacheline 0x{:x} from thread {}", 
                 cacheline_addr, source_thread_id);
}

bool ThreadPerConnectionServer::check_and_apply_back_invalidations(uint64_t cacheline_addr, int requesting_thread_id,
                                                                 CachelineInfo& info) {
    std::unique_lock<std::shared_mutex> lock(back_invalidation_mutex);
    
    auto it = back_invalidation_queue.find(cacheline_addr);
    if (it == back_invalidation_queue.end() || it->second.empty()) {
        return false;  // No back invalidations pending
    }
    
    bool had_back_invalidation = false;
    
    // Process all pending back invalidations for this cacheline
    while (!it->second.empty()) {
        BackInvalidationEntry& entry = it->second.front();
        
        // Apply the dirty data if it's newer than our current data
        if (entry.timestamp > info.last_access_time) {
            // Write the dirty data directly to shared memory
            // Calculate the actual address from cacheline base
            uint64_t write_addr = cacheline_addr;  // Start of cacheline
            
            if (!shm_manager->write_cacheline(write_addr, entry.dirty_data.data(), 
                                             entry.dirty_data.size())) {
                SPDLOG_ERROR("Failed to apply back invalidation to shared memory at 0x{:x}", 
                            write_addr);
            } else {
                info.has_dirty_update = false;  // Clear the flag after applying
                info.dirty_update_time = entry.timestamp;
                
                back_invalidations++;
                had_back_invalidation = true;
                
                SPDLOG_DEBUG("Applied back invalidation for cacheline 0x{:x} from thread {} to thread {}", 
                            cacheline_addr, entry.source_thread_id, requesting_thread_id);
            }
        }
        
        it->second.pop();
    }
    
    // Clean up empty queue
    if (it->second.empty()) {
        back_invalidation_queue.erase(it);
    }
    
    return had_back_invalidation;
}

// Shared memory mode implementation
void ThreadPerConnectionServer::run_shm_mode() {
    SPDLOG_INFO("Running in shared memory communication mode");
    
    // Create worker threads for handling SHM requests
    const int num_workers = 4;  // Configurable number of worker threads
    std::vector<std::thread> workers;
    
    for (int i = 0; i < num_workers; i++) {
        workers.emplace_back(&ThreadPerConnectionServer::handle_shm_requests, this);
    }
    
    // Wait for workers to finish (they won't unless stopped)
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPerConnectionServer::handle_shm_requests() {
    while (running) {
        uint32_t client_id;
        ShmRequest shm_req;
        
        // Wait for request with 100ms timeout
        if (!shm_comm_manager->wait_for_request(client_id, shm_req, 100)) {
            continue;
        }
        
        // Convert ShmRequest to ServerRequest format
        ServerRequest req;
        req.op_type = shm_req.op_type;
        req.addr = shm_req.addr;
        req.size = shm_req.size;
        req.timestamp = shm_req.timestamp;
        std::memcpy(req.data, shm_req.data, sizeof(req.data));
        
        // Handle special request for shared memory info
        if (req.op_type == 2) {  // GET_SHM_INFO
            ShmResponse shm_resp = {0};
            auto shm_info = shm_manager->get_shm_info();
            
            // For SHM mode, we return the info in the data field
            // Format: first 8 bytes = base_addr, next 8 = size, next 8 = num_cachelines
            uint64_t* data_ptr = reinterpret_cast<uint64_t*>(shm_resp.data);
            data_ptr[0] = shm_info.base_addr;
            data_ptr[1] = shm_info.size;
            data_ptr[2] = shm_info.num_cachelines;
            shm_resp.status = 0;
            
            shm_comm_manager->send_response(client_id, shm_resp);
            continue;
        }
        
        // Process regular request
        ServerResponse resp = {0};
        handle_request(-1, client_id, req, resp);  // Use client_id as thread_id
        
        // Convert ServerResponse to ShmResponse
        ShmResponse shm_resp;
        shm_resp.status = resp.status;
        shm_resp.latency_ns = resp.latency_ns;
        std::memcpy(shm_resp.data, resp.data, sizeof(shm_resp.data));
        
        // Send response back
        shm_comm_manager->send_response(client_id, shm_resp);
    }
}
