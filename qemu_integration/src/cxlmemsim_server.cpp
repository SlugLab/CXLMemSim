#include <iostream>
#include <thread>
#include <vector>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <chrono>
#include <atomic>
#include <map>
#include <mutex>
#include <signal.h>
#include "../../include/cxlcontroller.h"
#include "../../include/cxlendpoint.h"
#include "../include/qemu_cxl_memsim.h"

class CXLMemSimServer {
private:
    int server_fd;
    int port;
    std::unique_ptr<CXLController> controller;
    std::map<uint64_t, std::vector<uint8_t>> memory_storage;
    std::mutex memory_mutex;
    std::atomic<bool> running;
    
    struct AccessStats {
        uint64_t count;
        uint64_t last_access_time;
    };
    std::map<uint64_t, AccessStats> cacheline_stats;
    std::mutex stats_mutex;

public:
    CXLMemSimServer(int port, const std::string& topology_file) 
        : port(port), running(true) {
        controller = std::make_unique<CXLController>(
            topology_file,
            1,      // verbosity
            100,    // default latency
            64,     // cacheline mode
            1024,   // bw_limit_gbps
            "none", // allocation_policy
            "none", // migration_policy
            "none", // paging_policy
            "none", // caching_policy
            10.0,   // epoch_ms
            100,    // capacity_gb
            "ddr5", // memory_type
            ""      // output_file
        );
    }
    
    bool start() {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            std::cerr << "Failed to create socket" << std::endl;
            return false;
        }
        
        int opt = 1;
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            std::cerr << "Failed to set socket options" << std::endl;
            return false;
        }
        
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);
        
        if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            std::cerr << "Failed to bind to port " << port << std::endl;
            return false;
        }
        
        if (listen(server_fd, 10) < 0) {
            std::cerr << "Failed to listen on socket" << std::endl;
            return false;
        }
        
        std::cout << "CXLMemSim server listening on port " << port << std::endl;
        return true;
    }
    
    void handle_client(int client_fd) {
        std::cout << "Client connected" << std::endl;
        
        while (running) {
            CXLMemSimRequest req;
            ssize_t received = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
            
            if (received != sizeof(req)) {
                if (received == 0) {
                    std::cout << "Client disconnected" << std::endl;
                } else {
                    std::cerr << "Failed to receive request" << std::endl;
                }
                break;
            }
            
            CXLMemSimResponse resp = {0};
            
            // Use CXLController to calculate latency
            uint64_t latency_ns = controller->calculateLatency(req.addr, req.size, 
                req.op_type == CXL_READ_OP ? OP_READ : OP_WRITE);
            
            // Handle memory operation
            uint64_t cacheline_addr = req.addr & ~(CACHELINE_SIZE - 1);
            
            {
                std::lock_guard<std::mutex> lock(memory_mutex);
                
                if (req.op_type == CXL_READ_OP) {
                    // Read operation
                    auto it = memory_storage.find(cacheline_addr);
                    if (it != memory_storage.end()) {
                        size_t offset = req.addr - cacheline_addr;
                        memcpy(resp.data, it->second.data() + offset, req.size);
                    } else {
                        // Return zeros for uninitialized memory
                        memset(resp.data, 0, req.size);
                    }
                } else {
                    // Write operation
                    auto& cacheline = memory_storage[cacheline_addr];
                    if (cacheline.empty()) {
                        cacheline.resize(CACHELINE_SIZE, 0);
                    }
                    size_t offset = req.addr - cacheline_addr;
                    memcpy(cacheline.data() + offset, req.data, req.size);
                }
            }
            
            // Update statistics
            {
                std::lock_guard<std::mutex> lock(stats_mutex);
                auto& stats = cacheline_stats[cacheline_addr];
                stats.count++;
                stats.last_access_time = req.timestamp;
            }
            
            // Send response
            resp.status = 0;
            resp.latency_ns = latency_ns;
            
            ssize_t sent = send(client_fd, &resp, sizeof(resp), 0);
            if (sent != sizeof(resp)) {
                std::cerr << "Failed to send response" << std::endl;
                break;
            }
        }
        
        close(client_fd);
    }
    
    void run() {
        while (running) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            
            int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                if (running) {
                    std::cerr << "Failed to accept connection" << std::endl;
                }
                continue;
            }
            
            std::thread client_thread(&CXLMemSimServer::handle_client, this, client_fd);
            client_thread.detach();
        }
    }
    
    void stop() {
        running = false;
        close(server_fd);
    }
    
    void print_stats() {
        std::lock_guard<std::mutex> lock(stats_mutex);
        
        std::cout << "\nCacheline Access Statistics:" << std::endl;
        std::cout << "Total cachelines accessed: " << cacheline_stats.size() << std::endl;
        
        // Find top accessed cachelines
        std::vector<std::pair<uint64_t, uint64_t>> sorted_stats;
        for (const auto& [addr, stats] : cacheline_stats) {
            sorted_stats.push_back({stats.count, addr});
        }
        
        std::sort(sorted_stats.begin(), sorted_stats.end(), std::greater<>());
        
        std::cout << "\nTop 10 accessed cachelines:" << std::endl;
        size_t count = 0;
        for (const auto& [accesses, addr] : sorted_stats) {
            std::cout << "  0x" << std::hex << addr << std::dec 
                     << ": " << accesses << " accesses" << std::endl;
            if (++count >= 10) break;
        }
    }
};

static CXLMemSimServer* g_server = nullptr;

void signal_handler(int sig) {
    if (g_server) {
        g_server->print_stats();
        g_server->stop();
    }
    exit(0);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <port> <topology_file>" << std::endl;
        return 1;
    }
    
    int port = std::atoi(argv[1]);
    std::string topology_file = argv[2];
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    try {
        CXLMemSimServer server(port, topology_file);
        g_server = &server;
        
        if (!server.start()) {
            return 1;
        }
        
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}