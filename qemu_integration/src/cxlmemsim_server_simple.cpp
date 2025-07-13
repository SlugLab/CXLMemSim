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
#include <algorithm>
#include <random>
#include <csignal>
#include "../include/qemu_cxl_memsim.h"

class CXLMemSimServer {
private:
    int server_fd;
    int port;
    std::map<uint64_t, std::vector<uint8_t>> memory_storage;
    std::mutex memory_mutex;
    std::atomic<bool> running;
    
    // Configurable latency parameters
    double base_read_latency_ns;
    double base_write_latency_ns;
    double bandwidth_gbps;
    
    struct AccessStats {
        uint64_t count;
        uint64_t last_access_time;
    };
    std::map<uint64_t, AccessStats> cacheline_stats;
    std::mutex stats_mutex;

public:
    CXLMemSimServer(int port) 
        : port(port), running(true),
          base_read_latency_ns(200.0),  // CXL typical read latency
          base_write_latency_ns(100.0),  // CXL typical write latency
          bandwidth_gbps(64.0) {         // CXL 2.0 x8 bandwidth
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
        std::cout << "Configuration:" << std::endl;
        std::cout << "  Read Latency: " << base_read_latency_ns << " ns" << std::endl;
        std::cout << "  Write Latency: " << base_write_latency_ns << " ns" << std::endl;
        std::cout << "  Bandwidth: " << bandwidth_gbps << " GB/s" << std::endl;
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
            
            if (req.op_type == CXL_READ_OP) {
                resp.latency_ns = handle_read(req.addr, resp.data, req.size, req.timestamp);
                resp.status = 0;
            } else if (req.op_type == CXL_WRITE_OP) {
                resp.latency_ns = handle_write(req.addr, req.data, req.size, req.timestamp);
                resp.status = 0;
            } else {
                resp.status = 1;
            }
            
            ssize_t sent = send(client_fd, &resp, sizeof(resp), 0);
            if (sent != sizeof(resp)) {
                std::cerr << "Failed to send response" << std::endl;
                break;
            }
        }
        
        close(client_fd);
    }
    
    uint64_t calculate_latency(size_t size, bool is_read) {
        // Base latency
        double latency = is_read ? base_read_latency_ns : base_write_latency_ns;
        
        // Add bandwidth-based latency
        double transfer_time_ns = (size * 8.0) / (bandwidth_gbps * 1e9) * 1e9;
        latency += transfer_time_ns;
        
        // Add some variance based on queue depth (simplified)
        static thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_real_distribution<> dis(0.9, 1.1);
        latency *= dis(gen);
        
        return static_cast<uint64_t>(latency);
    }
    
    uint64_t handle_read(uint64_t addr, uint8_t* data, size_t size, uint64_t timestamp) {
        update_cacheline_stats(addr);
        
        memory_mutex.lock();
        auto it = memory_storage.find(addr);
        if (it != memory_storage.end() && it->second.size() >= size) {
            memcpy(data, it->second.data(), size);
        } else {
            // Initialize with zeros if not previously written
            memset(data, 0, size);
        }
        memory_mutex.unlock();
        
        return calculate_latency(size, true);
    }
    
    uint64_t handle_write(uint64_t addr, const uint8_t* data, size_t size, uint64_t timestamp) {
        update_cacheline_stats(addr);
        
        memory_mutex.lock();
        auto& storage = memory_storage[addr];
        storage.resize(size);
        memcpy(storage.data(), data, size);
        memory_mutex.unlock();
        
        return calculate_latency(size, false);
    }
    
    void update_cacheline_stats(uint64_t addr) {
        uint64_t cacheline_addr = addr & ~(CACHELINE_SIZE - 1);
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        
        std::lock_guard<std::mutex> lock(stats_mutex);
        auto& stats = cacheline_stats[cacheline_addr];
        stats.count++;
        stats.last_access_time = now;
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
    
    void print_hotness_report() {
        std::lock_guard<std::mutex> lock(stats_mutex);
        std::cout << "\n=== Cacheline Hotness Report ===" << std::endl;
        
        std::vector<std::pair<uint64_t, AccessStats>> sorted_stats;
        for (const auto& entry : cacheline_stats) {
            sorted_stats.push_back(entry);
        }
        
        std::sort(sorted_stats.begin(), sorted_stats.end(),
            [](const auto& a, const auto& b) {
                return a.second.count > b.second.count;
            });
        
        std::cout << "Top 20 Hottest Cachelines:" << std::endl;
        size_t count = 0;
        for (const auto& entry : sorted_stats) {
            if (count++ >= 20) break;
            std::cout << "  Address: 0x" << std::hex << entry.first 
                     << " - Accesses: " << std::dec << entry.second.count << std::endl;
        }
        
        std::cout << "Total unique cachelines accessed: " << cacheline_stats.size() << std::endl;
        
        // Calculate total accesses
        uint64_t total_accesses = 0;
        for (const auto& entry : cacheline_stats) {
            total_accesses += entry.second.count;
        }
        std::cout << "Total cacheline accesses: " << total_accesses << std::endl;
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return 1;
    }
    
    int port = std::atoi(argv[1]);
    
    CXLMemSimServer server(port);
    
    if (!server.start()) {
        return 1;
    }
    
    // Setup signal handler for graceful shutdown
    std::signal(SIGINT, [](int) {
        std::cout << "\nShutting down server..." << std::endl;
        exit(0);
    });
    
    // Start periodic reporting thread
    std::thread report_thread([&server]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            server.print_hotness_report();
        }
    });
    report_thread.detach();
    
    server.run();
    
    return 0;
}