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
#include "../../include/rdma_communication.h"

// Cache coherency states (MESI protocol)
enum CacheState {
    MESI_INVALID = 0,
    MESI_SHARED = 1,
    MESI_EXCLUSIVE = 2,
    MESI_MODIFIED = 3
};

// Memory entry structure (128 bytes total)
struct CXLMemoryEntry {
    // Data portion (64 bytes)
    uint8_t data[CACHELINE_SIZE];

    // Metadata portion (64 bytes)
    struct {
        uint8_t cache_state;        // MESI state
        uint8_t owner_id;           // Current owner host ID
        uint16_t sharers_bitmap;    // Bitmap of hosts sharing this line
        uint32_t access_count;      // Number of accesses
        uint64_t last_access_time;  // Timestamp of last access
        uint64_t virtual_addr;      // Virtual address mapping
        uint64_t physical_addr;     // Physical address
        uint32_t version;           // Version number for coherency
        uint8_t flags;              // Various flags (dirty, locked, etc.)
        uint8_t reserved[23];       // Reserved for future use
    } metadata;
};

class CXLMemSimServerRDMA {
private:
    // TCP server components
    int tcp_server_fd;
    int tcp_port;

    // RDMA server components
    std::unique_ptr<RDMAServer> rdma_server;
    int rdma_port;

    // Shared memory state
    std::map<uint64_t, CXLMemoryEntry> memory_storage;
    std::mutex memory_mutex;
    std::atomic<bool> running;

    // Virtual to physical address mapping
    std::map<std::pair<uint8_t, uint64_t>, uint64_t> virt_to_phys_map;
    std::mutex mapping_mutex;

    // Configurable latency parameters
    double base_read_latency_ns;
    double base_write_latency_ns;
    double bandwidth_gbps;

    // Transport mode
    RDMATransport::Mode transport_mode;

    struct AccessStats {
        uint64_t count;
        uint64_t last_access_time;
    };
    std::map<uint64_t, AccessStats> cacheline_stats;
    std::mutex stats_mutex;

public:
    CXLMemSimServerRDMA(int tcp_port, int rdma_port = 0)
        : tcp_port(tcp_port), rdma_port(rdma_port ? rdma_port : tcp_port + 1000),
          running(true),
          base_read_latency_ns(200.0),  // CXL typical read latency
          base_write_latency_ns(100.0),  // CXL typical write latency
          bandwidth_gbps(64.0) {         // CXL 2.0 x8 bandwidth

        transport_mode = RDMATransport::get_transport_mode();
        std::cout << "Transport mode: ";
        switch (transport_mode) {
            case RDMATransport::MODE_RDMA:
                std::cout << "RDMA" << std::endl;
                break;
            case RDMATransport::MODE_SHM:
                std::cout << "Shared Memory" << std::endl;
                break;
            default:
                std::cout << "TCP" << std::endl;
                break;
        }
    }

    bool start() {
        bool success = true;

        // Start TCP server (always enabled for backward compatibility)
        if (!start_tcp_server()) {
            success = false;
        }

        // Start RDMA server if requested and available
        if (transport_mode == RDMATransport::MODE_RDMA) {
            if (RDMATransport::is_rdma_available()) {
                if (!start_rdma_server()) {
                    std::cerr << "Failed to start RDMA server, falling back to TCP" << std::endl;
                    transport_mode = RDMATransport::MODE_TCP;
                }
            } else {
                std::cerr << "RDMA not available, falling back to TCP" << std::endl;
                transport_mode = RDMATransport::MODE_TCP;
            }
        }

        std::cout << "CXLMemSim server configuration:" << std::endl;
        std::cout << "  Read Latency: " << base_read_latency_ns << " ns" << std::endl;
        std::cout << "  Write Latency: " << base_write_latency_ns << " ns" << std::endl;
        std::cout << "  Bandwidth: " << bandwidth_gbps << " GB/s" << std::endl;

        return success;
    }

    bool start_tcp_server() {
        tcp_server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (tcp_server_fd < 0) {
            std::cerr << "Failed to create TCP socket" << std::endl;
            return false;
        }

        int opt = 1;
        if (setsockopt(tcp_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            std::cerr << "Failed to set TCP socket options" << std::endl;
            return false;
        }

        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(tcp_port);

        if (bind(tcp_server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            std::cerr << "Failed to bind TCP to port " << tcp_port << std::endl;
            return false;
        }

        if (listen(tcp_server_fd, 10) < 0) {
            std::cerr << "Failed to listen on TCP socket" << std::endl;
            return false;
        }

        std::cout << "TCP server listening on port " << tcp_port << std::endl;
        return true;
    }

    bool start_rdma_server() {
        rdma_server = std::make_unique<RDMAServer>("0.0.0.0", rdma_port);

        // Set up RDMA message handler
        rdma_server->set_message_handler(
            [this](const RDMAMessage& recv_msg, RDMAMessage& send_msg) {
                handle_rdma_message(recv_msg, send_msg);
            }
        );

        if (rdma_server->start() < 0) {
            std::cerr << "Failed to start RDMA server" << std::endl;
            rdma_server.reset();
            return false;
        }

        std::cout << "RDMA server listening on port " << rdma_port << std::endl;
        return true;
    }

    void handle_rdma_message(const RDMAMessage& recv_msg, RDMAMessage& send_msg) {
        const RDMARequest& req = recv_msg.request;
        RDMAResponse& resp = send_msg.response;

        if (req.op_type == RDMA_OP_READ) {
            resp.latency_ns = handle_read(req.addr, resp.data, req.size,
                                         req.timestamp, req.host_id, req.virtual_addr);
            resp.status = 0;

            // Get current cache state
            memory_mutex.lock();
            auto& entry = memory_storage[req.addr];
            resp.cache_state = entry.metadata.cache_state;
            memory_mutex.unlock();

        } else if (req.op_type == RDMA_OP_WRITE) {
            resp.latency_ns = handle_write(req.addr, req.data, req.size,
                                          req.timestamp, req.host_id, req.virtual_addr);
            resp.status = 0;

            // Get current cache state
            memory_mutex.lock();
            auto& entry = memory_storage[req.addr];
            resp.cache_state = entry.metadata.cache_state;
            memory_mutex.unlock();

        } else {
            resp.status = 1;
            resp.latency_ns = 0;
        }
    }

    void handle_tcp_client(int client_fd) {
        std::cout << "TCP client connected" << std::endl;

        static std::atomic<uint8_t> next_host_id{1};
        uint8_t host_id = next_host_id.fetch_add(1);

        while (running) {
            CXLMemSimRequest req;
            ssize_t received = recv(client_fd, &req, sizeof(req), MSG_WAITALL);

            if (received != sizeof(req)) {
                if (received == 0) {
                    std::cout << "TCP client disconnected (Host " << (int)host_id << ")" << std::endl;
                } else {
                    std::cerr << "Failed to receive TCP request" << std::endl;
                }
                break;
            }

            CXLMemSimResponse resp = {0};

            if (req.op_type == CXL_READ_OP) {
                resp.latency_ns = handle_read(req.addr, resp.data, req.size,
                                             req.timestamp, host_id, req.addr);
                resp.status = 0;

                memory_mutex.lock();
                auto& entry = memory_storage[req.addr];
                resp.data[CACHELINE_SIZE - 1] = entry.metadata.cache_state;
                memory_mutex.unlock();

            } else if (req.op_type == CXL_WRITE_OP) {
                resp.latency_ns = handle_write(req.addr, req.data, req.size,
                                              req.timestamp, host_id, req.addr);
                resp.status = 0;

                memory_mutex.lock();
                auto& entry = memory_storage[req.addr];
                resp.data[CACHELINE_SIZE - 1] = entry.metadata.cache_state;
                memory_mutex.unlock();

            } else {
                resp.status = 1;
            }

            ssize_t sent = send(client_fd, &resp, sizeof(resp), 0);
            if (sent != sizeof(resp)) {
                std::cerr << "Failed to send TCP response" << std::endl;
                break;
            }
        }

        cleanup_host_mappings(host_id);
        close(client_fd);
    }

    void handle_rdma_client() {
        if (!rdma_server) return;

        std::cout << "Waiting for RDMA client..." << std::endl;

        if (rdma_server->accept_connection() == 0) {
            std::cout << "RDMA client connected" << std::endl;
            rdma_server->handle_client();
            std::cout << "RDMA client disconnected" << std::endl;
        }
    }

    uint64_t calculate_latency(size_t size, bool is_read, bool is_rdma = false) {
        // Base latency - RDMA has lower latency
        double latency = is_read ? base_read_latency_ns : base_write_latency_ns;

        if (is_rdma) {
            // RDMA has significantly lower latency
            latency *= 0.3; // 70% reduction for RDMA
        }

        // Add bandwidth-based latency
        double transfer_time_ns = (size * 8.0) / (bandwidth_gbps * 1e9) * 1e9;
        latency += transfer_time_ns;

        // Add some variance
        static thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_real_distribution<> dis(0.9, 1.1);
        latency *= dis(gen);

        return static_cast<uint64_t>(latency);
    }

    CacheState handle_coherency_transition(CXLMemoryEntry& entry, uint8_t requester_id, bool is_write) {
        CacheState old_state = static_cast<CacheState>(entry.metadata.cache_state);
        CacheState new_state = old_state;

        if (is_write) {
            switch (old_state) {
                case MESI_INVALID:
                case MESI_SHARED:
                case MESI_EXCLUSIVE:
                    new_state = MESI_MODIFIED;
                    entry.metadata.owner_id = requester_id;
                    entry.metadata.sharers_bitmap = (1 << requester_id);
                    break;
                case MESI_MODIFIED:
                    if (entry.metadata.owner_id != requester_id) {
                        new_state = MESI_MODIFIED;
                        entry.metadata.owner_id = requester_id;
                        entry.metadata.sharers_bitmap = (1 << requester_id);
                    }
                    break;
            }
        } else {
            switch (old_state) {
                case MESI_INVALID:
                    new_state = MESI_EXCLUSIVE;
                    entry.metadata.owner_id = requester_id;
                    entry.metadata.sharers_bitmap = (1 << requester_id);
                    break;
                case MESI_EXCLUSIVE:
                    if (entry.metadata.owner_id != requester_id) {
                        new_state = MESI_SHARED;
                        entry.metadata.sharers_bitmap |= (1 << requester_id);
                    }
                    break;
                case MESI_SHARED:
                    entry.metadata.sharers_bitmap |= (1 << requester_id);
                    break;
                case MESI_MODIFIED:
                    if (entry.metadata.owner_id != requester_id) {
                        new_state = MESI_SHARED;
                        entry.metadata.sharers_bitmap |= (1 << requester_id);
                    }
                    break;
            }
        }

        entry.metadata.cache_state = new_state;
        entry.metadata.version++;
        return new_state;
    }

    uint64_t handle_read(uint64_t addr, uint8_t* data, size_t size, uint64_t timestamp,
                        uint8_t host_id = 0, uint64_t virt_addr = 0) {
        update_cacheline_stats(addr);

        memory_mutex.lock();
        auto& entry = memory_storage[addr];

        if (entry.metadata.physical_addr == 0) {
            entry.metadata.physical_addr = addr;
            entry.metadata.cache_state = MESI_INVALID;
            memset(entry.data, 0, CACHELINE_SIZE);
        }

        if (virt_addr != 0) {
            mapping_mutex.lock();
            virt_to_phys_map[{host_id, virt_addr}] = addr;
            entry.metadata.virtual_addr = virt_addr;
            mapping_mutex.unlock();
        }

        CacheState new_state = handle_coherency_transition(entry, host_id, false);

        memcpy(data, entry.data, std::min(size, (size_t)CACHELINE_SIZE));

        entry.metadata.access_count++;
        entry.metadata.last_access_time = timestamp;

        memory_mutex.unlock();

        bool is_rdma = (transport_mode == RDMATransport::MODE_RDMA);
        uint64_t base_latency = calculate_latency(size, true, is_rdma);

        if (new_state == MESI_SHARED && entry.metadata.cache_state == MESI_MODIFIED) {
            base_latency += is_rdma ? 15 : 50;
        }

        return base_latency;
    }

    uint64_t handle_write(uint64_t addr, const uint8_t* data, size_t size, uint64_t timestamp,
                         uint8_t host_id = 0, uint64_t virt_addr = 0) {
        update_cacheline_stats(addr);

        memory_mutex.lock();
        auto& entry = memory_storage[addr];

        if (entry.metadata.physical_addr == 0) {
            entry.metadata.physical_addr = addr;
            entry.metadata.cache_state = MESI_INVALID;
        }

        if (virt_addr != 0) {
            mapping_mutex.lock();
            virt_to_phys_map[{host_id, virt_addr}] = addr;
            entry.metadata.virtual_addr = virt_addr;
            mapping_mutex.unlock();
        }

        CacheState old_state = static_cast<CacheState>(entry.metadata.cache_state);
        CacheState new_state = handle_coherency_transition(entry, host_id, true);

        memcpy(entry.data, data, std::min(size, (size_t)CACHELINE_SIZE));

        entry.metadata.access_count++;
        entry.metadata.last_access_time = timestamp;

        memory_mutex.unlock();

        bool is_rdma = (transport_mode == RDMATransport::MODE_RDMA);
        uint64_t base_latency = calculate_latency(size, false, is_rdma);

        if (old_state == MESI_SHARED || (old_state == MESI_MODIFIED && entry.metadata.owner_id != host_id)) {
            base_latency += is_rdma ? 30 : 100;
        }

        return base_latency;
    }

    void update_cacheline_stats(uint64_t addr) {
        uint64_t cacheline_addr = addr & ~(CACHELINE_SIZE - 1);
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();

        std::lock_guard<std::mutex> lock(stats_mutex);
        auto& stats = cacheline_stats[cacheline_addr];
        stats.count++;
        stats.last_access_time = now;
    }

    void cleanup_host_mappings(uint8_t host_id) {
        mapping_mutex.lock();
        auto it = virt_to_phys_map.begin();
        while (it != virt_to_phys_map.end()) {
            if (it->first.first == host_id) {
                it = virt_to_phys_map.erase(it);
            } else {
                ++it;
            }
        }
        mapping_mutex.unlock();
    }

    void run() {
        std::vector<std::thread> worker_threads;

        // TCP accept thread
        worker_threads.emplace_back([this]() {
            while (running) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);

                int client_fd = accept(tcp_server_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0) {
                    if (running) {
                        std::cerr << "Failed to accept TCP connection" << std::endl;
                    }
                    continue;
                }

                std::thread client_thread(&CXLMemSimServerRDMA::handle_tcp_client, this, client_fd);
                client_thread.detach();
            }
        });

        // RDMA accept thread (if enabled)
        if (rdma_server) {
            worker_threads.emplace_back([this]() {
                while (running) {
                    handle_rdma_client();
                }
            });
        }

        // Wait for all threads
        for (auto& t : worker_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    void stop() {
        running = false;
        close(tcp_server_fd);
        if (rdma_server) {
            rdma_server->stop();
        }
    }

    void print_report() {
        std::lock_guard<std::mutex> lock(stats_mutex);
        std::cout << "\n=== CXLMemSim Server Report ===" << std::endl;
        std::cout << "Transport Mode: ";
        switch (transport_mode) {
            case RDMATransport::MODE_RDMA:
                std::cout << "RDMA (Port " << rdma_port << ")" << std::endl;
                break;
            case RDMATransport::MODE_SHM:
                std::cout << "Shared Memory" << std::endl;
                break;
            default:
                std::cout << "TCP (Port " << tcp_port << ")" << std::endl;
                break;
        }

        std::vector<std::pair<uint64_t, AccessStats>> sorted_stats;
        for (const auto& entry : cacheline_stats) {
            sorted_stats.push_back(entry);
        }

        std::sort(sorted_stats.begin(), sorted_stats.end(),
            [](const auto& a, const auto& b) {
                return a.second.count > b.second.count;
            });

        std::cout << "\nTop 10 Hottest Cachelines:" << std::endl;
        size_t count = 0;

        memory_mutex.lock();
        for (const auto& entry : sorted_stats) {
            if (count++ >= 10) break;

            auto mem_it = memory_storage.find(entry.first);
            if (mem_it != memory_storage.end()) {
                const auto& mem_entry = mem_it->second;
                const char* state_str = "INVALID";
                switch (mem_entry.metadata.cache_state) {
                    case MESI_SHARED: state_str = "SHARED"; break;
                    case MESI_EXCLUSIVE: state_str = "EXCLUSIVE"; break;
                    case MESI_MODIFIED: state_str = "MODIFIED"; break;
                }

                std::cout << "  0x" << std::hex << entry.first
                         << " - " << std::dec << entry.second.count << " accesses"
                         << " - " << state_str
                         << " - Host" << (int)mem_entry.metadata.owner_id << std::endl;
            }
        }
        memory_mutex.unlock();

        std::cout << "\nTotal unique cachelines: " << cacheline_stats.size() << std::endl;

        uint64_t total_accesses = 0;
        for (const auto& entry : cacheline_stats) {
            total_accesses += entry.second.count;
        }
        std::cout << "Total accesses: " << total_accesses << std::endl;
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <tcp_port> [rdma_port]" << std::endl;
        std::cerr << "Environment variables:" << std::endl;
        std::cerr << "  CXL_TRANSPORT_MODE=rdma|shm|tcp (default: tcp)" << std::endl;
        return 1;
    }

    int tcp_port = std::atoi(argv[1]);
    int rdma_port = argc > 2 ? std::atoi(argv[2]) : 0;

    CXLMemSimServerRDMA server(tcp_port, rdma_port);

    if (!server.start()) {
        return 1;
    }

    // Signal handler for graceful shutdown
    std::signal(SIGINT, [](int) {
        std::cout << "\nShutting down server..." << std::endl;
        exit(0);
    });

    // Periodic reporting thread
    std::thread report_thread([&server]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            server.print_report();
        }
    });
    report_thread.detach();

    server.run();

    return 0;
}