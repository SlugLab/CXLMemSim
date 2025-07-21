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
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <set>
#include <fstream>
#include <iterator>

#ifndef MSG_WAITALL
#define MSG_WAITALL 0x100
#endif

// Global variables
Helper helper{};
CXLController* controller = nullptr;

// Server request/response structures (matching qemu_integration)
struct ServerRequest {
    uint8_t op_type;      // 0=READ, 1=WRITE
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

// Coherency states
enum CoherencyState {
    INVALID,
    SHARED,
    EXCLUSIVE,
    MODIFIED
};

// Cacheline coherency information
struct CachelineInfo {
    CoherencyState state;
    std::set<int> sharers;      // Thread IDs that have this cacheline in SHARED state
    int owner;                  // Thread ID that owns in EXCLUSIVE/MODIFIED state
    uint64_t last_access_time;
    std::vector<uint8_t> data;  // Actual data
    
    CachelineInfo() : state(INVALID), owner(-1), last_access_time(0) {
        data.resize(64, 0);
    }
};

// Thread-per-connection server class
class ThreadPerConnectionServer {
private:
    int server_fd;
    int port;
    CXLController* controller;
    std::atomic<bool> running;
    std::atomic<int> next_thread_id;
    
    // Memory storage with coherency tracking
    std::map<uint64_t, CachelineInfo> cacheline_storage;  // Cacheline address -> info
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
    
    // Statistics
    std::atomic<uint64_t> total_reads{0};
    std::atomic<uint64_t> total_writes{0};
    std::atomic<uint64_t> coherency_invalidations{0};
    std::atomic<uint64_t> coherency_downgrades{0};
    
public:
    ThreadPerConnectionServer(int port, CXLController* ctrl)
        : port(port), controller(ctrl), running(true), next_thread_id(0) {
        congestion_info.active_requests = 0;
        congestion_info.total_bandwidth_used = 0;
        congestion_info.last_reset = std::chrono::steady_clock::now();
    }
    
    bool start();
    void run();
    void stop();
    void handle_client(int client_fd);
    
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
        ("capacity", "Capacity of CXL expander in GB", cxxopts::value<int>()->default_value("2"))
        ("p,port", "Server port", cxxopts::value<int>()->default_value("9999"))
        ("t,topology", "Topology file", cxxopts::value<std::string>()->default_value("topology.txt"));

    auto result = options.parse(argc, argv);
    
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
    
    SPDLOG_INFO("CXLMemSim Thread-per-Connection Server starting on port {}", port);
    SPDLOG_INFO("Topology: {}", topology);
    SPDLOG_INFO("Capacity: {} GB", capacity);
    SPDLOG_INFO("Default latency: {} ns", default_latency);
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    try {
        ThreadPerConnectionServer server(port, controller);
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
            client_threads.emplace_back(&ThreadPerConnectionServer::handle_client, this, client_fd);
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
    switch (info.state) {
        case INVALID:
            info.state = MODIFIED;
            info.owner = thread_id;
            break;
            
        case SHARED:
            // Invalidate all sharers
            invalidate_sharers(cacheline_addr, thread_id, info);
            info.state = MODIFIED;
            info.owner = thread_id;
            info.sharers.clear();
            break;
            
        case EXCLUSIVE:
        case MODIFIED:
            if (info.owner != thread_id) {
                // Invalidate current owner
                invalidate_sharers(cacheline_addr, thread_id, info);
            }
            info.state = MODIFIED;
            info.owner = thread_id;
            break;
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
    
    // Increment active requests
    congestion_info.active_requests++;
    
    // Calculate base latency using CXL controller
    std::vector<std::tuple<uint64_t, uint64_t>> access_elem;
    access_elem.push_back(std::make_tuple(req.addr, req.size));
    double base_latency = controller->calculate_latency(access_elem, controller->dramlatency);
    
    // Handle coherency and memory operation
    {
        std::unique_lock<std::shared_mutex> lock(memory_mutex);
        auto& info = cacheline_storage[cacheline_addr];
        
        // Check if we need coherency actions
        if (req.op_type == 0) {  // READ
            if (info.state == EXCLUSIVE || info.state == MODIFIED) {
                if (info.owner != -1 && info.owner != thread_id) {
                    had_coherency_miss = true;
                }
            }
            handle_read_coherency(cacheline_addr, thread_id, info);
            
            // Copy data
            size_t offset = req.addr - cacheline_addr;
            memcpy(resp.data, info.data.data() + offset, req.size);
            
            total_reads++;
        } else {  // WRITE
            if (info.state == SHARED && !info.sharers.empty()) {
                had_coherency_miss = true;
            } else if ((info.state == EXCLUSIVE || info.state == MODIFIED) && 
                      info.owner != thread_id) {
                had_coherency_miss = true;
            }
            handle_write_coherency(cacheline_addr, thread_id, info);
            
            // Update data
            size_t offset = req.addr - cacheline_addr;
            memcpy(info.data.data() + offset, req.data, req.size);
            
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
    
    SPDLOG_DEBUG("Thread {}: {} addr 0x{:x}, size {}, latency {}ns (base: {}ns, congestion: {:.2f}x, coherency_miss: {})",
                thread_id, req.op_type == 0 ? "READ" : "WRITE", req.addr, req.size, 
                total_latency, static_cast<uint64_t>(base_latency), congestion_factor, had_coherency_miss);
}

void ThreadPerConnectionServer::handle_client(int client_fd) {
    int thread_id = next_thread_id - 1;  // Get our thread ID
    SPDLOG_INFO("Thread {}: Client connected", thread_id);
    
    while (running) {
        ServerRequest req;
        ssize_t received = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
        
        if (received != sizeof(req)) {
            if (received == 0) {
                SPDLOG_INFO("Thread {}: Client disconnected", thread_id);
            } else {
                SPDLOG_ERROR("Thread {}: Failed to receive request", thread_id);
            }
            break;
        }
        
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