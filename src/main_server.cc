/*
 * CXLMemSim controller - Multi-Port Server Mode
 * Multi-threaded server with one thread per port, shared CXLController,
 * coherency protocol, and reorder buffer (RoB)
 *
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "cxlcontroller.h"
#include "cxlendpoint.h"
#include "helper.h"
#include "policy.h"
#ifndef SERVER_MODE
#include "monitor.h"
#endif
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

#ifndef MSG_WAITALL
#define MSG_WAITALL 0x100
#endif

// Global variables
Helper helper{};
std::shared_ptr<CXLController> controller;
#ifndef SERVER_MODE
Monitors *monitors;
#endif

// Server request/response structures
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

// Reorder Buffer entry
struct RoBEntry {
    uint64_t addr;
    uint64_t timestamp;
    int thread_id;
    bool is_write;
    bool completed;
    std::vector<uint8_t> data;
    uint64_t latency_ns;
};

// Multi-port server class
class MultiPortServer {
private:
    std::vector<int> server_fds;
    std::vector<int> ports;
    std::shared_ptr<CXLController> controller;
    std::map<uint64_t, std::vector<uint8_t>> memory_storage;
    std::shared_mutex memory_mutex;
    std::atomic<bool> running;
    
    // Coherency tracking
    std::map<uint64_t, int> cacheline_ownership;  // cacheline -> thread_id
    std::shared_mutex coherency_mutex;
    
    // Reorder Buffer - one queue per thread
    std::vector<std::queue<RoBEntry>> rob_queues;
    std::vector<std::unique_ptr<std::mutex>> rob_mutexes;
    std::vector<std::unique_ptr<std::condition_variable>> rob_cvs;
    
    // Thread pool
    std::vector<std::thread> worker_threads;
    
public:
    MultiPortServer(const std::vector<int>& ports, std::shared_ptr<CXLController> ctrl)
        : ports(ports), controller(ctrl), running(true) {
        server_fds.resize(ports.size());
        rob_queues.resize(ports.size());
        
        // Initialize mutexes and condition variables
        for (size_t i = 0; i < ports.size(); i++) {
            rob_mutexes.push_back(std::make_unique<std::mutex>());
            rob_cvs.push_back(std::make_unique<std::condition_variable>());
        }
    }
    
    bool start();
    void run();
    void stop();
    void handle_coherency(uint64_t cacheline_addr, int thread_id, bool is_write);
    void process_rob_entries(int thread_id);
    void handle_request(int client_fd, int thread_id, ServerRequest& req);
    void handle_client(int client_fd, int thread_id);
    void worker_thread_func(int thread_id);
};

static MultiPortServer* g_server = nullptr;

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
    cxxopts::Options options("CXLMemSim Server", "CXL.mem Type 3 Memory Controller Multi-Port Server");
    
    options.add_options()
        ("h,help", "Help for CXLMemSim Server", cxxopts::value<bool>()->default_value("false"))
        ("v,verbose", "Verbose level", cxxopts::value<int>()->default_value("2"))
        ("default_latency", "Default latency", cxxopts::value<size_t>()->default_value("100"))
        ("interleave_size", "Interleave size", cxxopts::value<size_t>()->default_value("256"))
        ("capacity", "Capacity of CXL expander in GB", cxxopts::value<int>()->default_value("2"))
        ("p,ports", "Server ports (comma-separated)", cxxopts::value<std::string>()->default_value("9999"))
        ("t,topology", "Topology file", cxxopts::value<std::string>()->default_value("topology.txt"))
        ("n,num-threads", "Number of threads/ports", cxxopts::value<int>()->default_value("4"));

    auto result = options.parse(argc, argv);
    
    if (result.count("help")) {
        fmt::print("{}\n", options.help());
        return 0;
    }

    int verbose = result["verbose"].as<int>();
    size_t default_latency = result["default_latency"].as<size_t>();
    size_t interleave_size = result["interleave_size"].as<size_t>();
    int capacity = result["capacity"].as<int>();
    std::string ports_str = result["ports"].as<std::string>();
    std::string topology = result["topology"].as<std::string>();
    int num_threads = result["num-threads"].as<int>();
    
    // Parse ports
    std::vector<int> ports;
    if (ports_str.find(',') != std::string::npos) {
        // Multiple ports specified
        std::stringstream ss(ports_str);
        std::string port;
        while (std::getline(ss, port, ',')) {
            ports.push_back(std::stoi(port));
        }
    } else {
        // Single port specified, create consecutive ports
        int base_port = std::stoi(ports_str);
        for (int i = 0; i < num_threads; i++) {
            ports.push_back(base_port + i);
        }
    }

    // Initialize policies
    std::array<Policy *, 4> policies = {
        new AllocationPolicy(),
        new MigrationPolicy(),
        new PagingPolicy(),
        new CachingPolicy()
    };

    // Create shared controller
    controller = std::make_shared<CXLController>(policies, capacity, PAGE, 10, default_latency);
    
    SPDLOG_INFO("CXLMemSim Multi-Port Server starting with {} ports", ports.size());
    for (size_t i = 0; i < ports.size(); i++) {
        SPDLOG_INFO("Thread {} will listen on port {}", i, ports[i]);
    }
    SPDLOG_INFO("Topology: {}", topology);
    SPDLOG_INFO("Capacity: {} GB", capacity);
    SPDLOG_INFO("Default latency: {} ns", default_latency);
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    try {
        MultiPortServer server(ports, controller);
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

// MultiPortServer implementation
bool MultiPortServer::start() {
    for (size_t i = 0; i < ports.size(); i++) {
        server_fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fds[i] < 0) {
            SPDLOG_ERROR("Failed to create socket for port {}", ports[i]);
            return false;
        }
        
        int opt = 1;
        if (setsockopt(server_fds[i], SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            SPDLOG_ERROR("Failed to set socket options for port {}", ports[i]);
            return false;
        }
        
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(ports[i]);
        
        if (bind(server_fds[i], (struct sockaddr *)&address, sizeof(address)) < 0) {
            SPDLOG_ERROR("Failed to bind to port {}", ports[i]);
            return false;
        }
        
        if (listen(server_fds[i], 10) < 0) {
            SPDLOG_ERROR("Failed to listen on port {}", ports[i]);
            return false;
        }
        
        SPDLOG_INFO("Thread {} listening on port {}", i, ports[i]);
    }
    return true;
}

void MultiPortServer::handle_coherency(uint64_t cacheline_addr, int thread_id, bool is_write) {
    std::unique_lock<std::shared_mutex> lock(coherency_mutex);
    
    auto it = cacheline_ownership.find(cacheline_addr);
    if (it != cacheline_ownership.end() && it->second != thread_id) {
        // Another thread owns this cacheline - invalidate it
        SPDLOG_DEBUG("Thread {} requesting cacheline 0x{:x} from thread {}", 
                    thread_id, cacheline_addr, it->second);
    }
    
    if (is_write) {
        // Write operation takes exclusive ownership
        cacheline_ownership[cacheline_addr] = thread_id;
    }
}

void MultiPortServer::process_rob_entries(int thread_id) {
    std::unique_lock<std::mutex> lock(*rob_mutexes[thread_id]);
    
    while (!rob_queues[thread_id].empty() && 
           rob_queues[thread_id].front().completed) {
        RoBEntry& entry = rob_queues[thread_id].front();
        
        // Commit write operations
        if (entry.is_write) {
            std::unique_lock<std::shared_mutex> mem_lock(memory_mutex);
            uint64_t cacheline_addr = entry.addr & ~(63ULL);  // 64-byte cacheline
            auto& cacheline = memory_storage[cacheline_addr];
            if (cacheline.empty()) {
                cacheline.resize(64, 0);
            }
            size_t offset = entry.addr - cacheline_addr;
            memcpy(cacheline.data() + offset, entry.data.data(), entry.data.size());
        }
        
        rob_queues[thread_id].pop();
        rob_cvs[thread_id]->notify_all();
    }
}

void MultiPortServer::handle_request(int client_fd, int thread_id, ServerRequest& req) {
    ServerResponse resp = {0};
    uint64_t cacheline_addr = req.addr & ~(63ULL);
    
    // Create RoB entry
    RoBEntry rob_entry;
    rob_entry.addr = req.addr;
    rob_entry.timestamp = req.timestamp;
    rob_entry.thread_id = thread_id;
    rob_entry.is_write = (req.op_type == 1);  // 1 = WRITE
    rob_entry.completed = false;
    
    // Add to RoB
    {
        std::unique_lock<std::mutex> lock(*rob_mutexes[thread_id]);
        rob_queues[thread_id].push(rob_entry);
    }
    
    // Handle coherency
    handle_coherency(cacheline_addr, thread_id, rob_entry.is_write);
    
    // Calculate latency using shared CXLController
    // Create a vector with address and size tuple for the controller
    std::vector<std::tuple<uint64_t, uint64_t>> access_elem;
    access_elem.push_back(std::make_tuple(req.addr, req.size));
    
    // Use controller's dram latency
    double latency_ns = controller->calculate_latency(access_elem, controller->dramlatency);
    
    // Handle memory operation
    if (req.op_type == 0) {  // READ
        std::shared_lock<std::shared_mutex> lock(memory_mutex);
        auto it = memory_storage.find(cacheline_addr);
        if (it != memory_storage.end()) {
            size_t offset = req.addr - cacheline_addr;
            memcpy(resp.data, it->second.data() + offset, req.size);
        } else {
            memset(resp.data, 0, req.size);
        }
    } else {  // WRITE
        rob_entry.data.resize(req.size);
        memcpy(rob_entry.data.data(), req.data, req.size);
    }
    
    // Mark RoB entry as completed
    {
        std::unique_lock<std::mutex> lock(*rob_mutexes[thread_id]);
        // Find and update our entry
        std::queue<RoBEntry> temp_queue;
        while (!rob_queues[thread_id].empty()) {
            RoBEntry& entry = rob_queues[thread_id].front();
            if (entry.addr == rob_entry.addr && 
                entry.timestamp == rob_entry.timestamp) {
                entry.completed = true;
                entry.latency_ns = latency_ns;
                if (!entry.data.empty()) {
                    entry.data = rob_entry.data;
                }
            }
            temp_queue.push(entry);
            rob_queues[thread_id].pop();
        }
        rob_queues[thread_id] = temp_queue;
    }
    
    // Process completed RoB entries
    process_rob_entries(thread_id);
    
    // Send response
    resp.status = 0;
    resp.latency_ns = latency_ns;
    
    ssize_t sent = send(client_fd, &resp, sizeof(resp), 0);
    if (sent != sizeof(resp)) {
        SPDLOG_ERROR("Thread {}: Failed to send response", thread_id);
    }
}

void MultiPortServer::handle_client(int client_fd, int thread_id) {
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
        
        handle_request(client_fd, thread_id, req);
    }
    
    close(client_fd);
}

void MultiPortServer::worker_thread_func(int thread_id) {
    SPDLOG_INFO("Worker thread {} started for port {}", thread_id, ports[thread_id]);
    
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_fds[thread_id], 
                              (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (running) {
                SPDLOG_ERROR("Thread {}: Failed to accept connection", thread_id);
            }
            continue;
        }
        
        // Handle client in the same thread
        handle_client(client_fd, thread_id);
    }
}

void MultiPortServer::run() {
    // Create one thread per port
    for (size_t i = 0; i < ports.size(); i++) {
        worker_threads.emplace_back(&MultiPortServer::worker_thread_func, this, i);
    }
    
    // Wait for all threads
    for (auto& thread : worker_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void MultiPortServer::stop() {
    running = false;
    for (int fd : server_fds) {
        close(fd);
    }
}