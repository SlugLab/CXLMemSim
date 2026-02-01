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
#include "monitor.h"
#include "policy.h"
#include "shared_memory_manager.h"
#include "shm_communication.h"
#include "cxl_backend.h"
#include "distributed_server.h"
#include <cerrno>
#include <cxxopts.hpp>
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <shared_mutex>
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
#include <fstream>
#include <iostream>
#include <sys/mman.h>  // For msync

#ifndef MSG_WAITALL
#define MSG_WAITALL 0x100
#endif

// Global variables
Helper helper{};
CXLController* controller = nullptr;
Monitors* monitors = nullptr;  // Required by helper.cpp signal handlers

// Operation type constants
constexpr uint8_t OP_READ = 0;
constexpr uint8_t OP_WRITE = 1;
constexpr uint8_t OP_GET_SHM_INFO = 2;
constexpr uint8_t OP_ATOMIC_FAA = 3;   // Fetch-and-Add
constexpr uint8_t OP_ATOMIC_CAS = 4;   // Compare-and-Swap
constexpr uint8_t OP_FENCE = 5;        // Memory fence

// Server request/response structures (matching qemu_integration)
struct __attribute__((packed)) ServerRequest {
    uint8_t op_type;      // 0=READ, 1=WRITE, 2=GET_SHM_INFO, 3=ATOMIC_FAA, 4=ATOMIC_CAS, 5=FENCE
    uint64_t addr;
    uint64_t size;
    uint64_t timestamp;
    uint64_t value;       // Value for FAA (add value) or CAS (desired value)
    uint64_t expected;    // Expected value for CAS operation
    uint8_t data[64];     // Cacheline data
};

struct __attribute__((packed)) ServerResponse {
    uint8_t status;
    uint64_t latency_ns;
    uint64_t old_value;   // Previous value returned by atomic operations
    uint8_t data[64];
};

// Extended response for shared memory info
struct __attribute__((packed)) SharedMemoryInfoResponse {
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
    SHM,        // Shared Memory via /dev/shm (ring buffer based)
    PGAS_SHM,   // PGAS Shared Memory (lock-free slots for cxl_backend.h clients)
    DISTRIBUTED // Distributed multi-node memory server
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

    // PGAS SHMEM backend state (for cxl_backend.h protocol)
    std::string pgas_shm_name_;
    int pgas_shm_fd_;
    cxl_shm_header_t* pgas_shm_header_;
    void* pgas_memory_;
    size_t pgas_memory_size_;

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
    std::atomic<uint64_t> total_atomic_faa{0};
    std::atomic<uint64_t> total_atomic_cas{0};
    std::atomic<uint64_t> total_atomic_cas_success{0};
    std::atomic<uint64_t> total_fences{0};
    std::atomic<uint64_t> coherency_invalidations{0};
    std::atomic<uint64_t> coherency_downgrades{0};
    std::atomic<uint64_t> back_invalidations{0};

    // Periodic logging interval
    static constexpr uint64_t LOG_INTERVAL = 100000;

    // Helper to log stats periodically
    void log_periodic_stats(const char* op_type, uint64_t count) {
        if (count % LOG_INTERVAL == 0) {
            uint64_t total_ops = total_reads + total_writes + total_atomic_faa +
                                total_atomic_cas + total_fences;
            SPDLOG_INFO("=== Stats @ {} {} ops ===", count, op_type);
            SPDLOG_INFO("  Total operations: {}", total_ops);
            SPDLOG_INFO("  Reads: {}, Writes: {}", total_reads.load(), total_writes.load());
            SPDLOG_INFO("  Atomics: FAA={}, CAS={} (success={}), Fences={}",
                       total_atomic_faa.load(), total_atomic_cas.load(),
                       total_atomic_cas_success.load(), total_fences.load());
            SPDLOG_INFO("  Coherency: invalidations={}, downgrades={}, back_inv={}",
                       coherency_invalidations.load(), coherency_downgrades.load(),
                       back_invalidations.load());
        }
    }
    
public:
    ThreadPerConnectionServer(int port, CXLController* ctrl, size_t capacity_mb,
                            const std::string& backing_file = "", CommMode mode = CommMode::TCP,
                            const std::string& pgas_shm_name = "/cxlmemsim_pgas")
        : port(port), controller(ctrl), running(true), next_thread_id(0),
          backing_file_(backing_file), comm_mode(mode),
          pgas_shm_name_(pgas_shm_name), pgas_shm_fd_(-1),
          pgas_shm_header_(nullptr), pgas_memory_(nullptr), pgas_memory_size_(0) {
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

    // PGAS Shared memory mode methods (cxl_backend.h protocol)
    bool init_pgas_shm(const std::string& shm_name, size_t memory_size);
    void run_pgas_shm_mode();
    int poll_pgas_shm_requests();
    void cleanup_pgas_shm();

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
    void handle_atomic_request(int thread_id, ServerRequest& req, ServerResponse& resp);
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
        ("comm-mode", "Communication mode: tcp, shm, pgas-shm, or distributed", cxxopts::value<std::string>()->default_value("tcp"))
        ("pgas-shm-name", "PGAS shared memory name (for pgas-shm mode)", cxxopts::value<std::string>()->default_value("/cxlmemsim_pgas"))
        ("node-id", "Node ID for distributed mode (0 = coordinator)", cxxopts::value<uint32_t>()->default_value("0"))
        ("dist-shm-name", "Shared memory name for distributed inter-node communication", cxxopts::value<std::string>()->default_value("/cxlmemsim_dist"))
        ("coordinator-shm", "Coordinator's shared memory name (for joining existing cluster)", cxxopts::value<std::string>()->default_value(""))
        ("transport-mode", "Transport mode for distributed: shm, tcp, or hybrid", cxxopts::value<std::string>()->default_value("shm"))
        ("tcp-addr", "TCP bind address for distributed TCP transport", cxxopts::value<std::string>()->default_value("0.0.0.0"))
        ("tcp-port", "TCP port for distributed TCP transport", cxxopts::value<uint16_t>()->default_value("5555"))
        ("tcp-peers", "Comma-separated list of TCP peer addresses (node_id:addr:port,...)", cxxopts::value<std::string>()->default_value(""));

    auto result = options.parse(argc, argv);
    
    std::string backing_file = result["backing-file"].as<std::string>();
    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    int verbose = result["verbose"].as<int>();
    size_t default_latency = result["default_latency"].as<size_t>();
    size_t interleave_size = result["interleave_size"].as<size_t>();
    int capacity = result["capacity"].as<int>();
    int port = result["port"].as<int>();
    std::string topology = result["topology"].as<std::string>();
    std::string comm_mode_str = result["comm-mode"].as<std::string>();
    
    std::string pgas_shm_name = result["pgas-shm-name"].as<std::string>();

    // Parse distributed mode options
    uint32_t node_id = result["node-id"].as<uint32_t>();
    std::string dist_shm_name = result["dist-shm-name"].as<std::string>();
    std::string coordinator_shm = result["coordinator-shm"].as<std::string>();

    // Parse TCP transport options
    std::string transport_mode_str = result["transport-mode"].as<std::string>();
    std::string tcp_addr = result["tcp-addr"].as<std::string>();
    uint16_t tcp_transport_port = result["tcp-port"].as<uint16_t>();
    std::string tcp_peers_str = result["tcp-peers"].as<std::string>();

    // Map transport mode string to enum
    DistTransportMode transport_mode = DistTransportMode::SHM;
    if (transport_mode_str == "tcp") {
        transport_mode = DistTransportMode::TCP;
    } else if (transport_mode_str == "hybrid") {
        transport_mode = DistTransportMode::HYBRID;
    } else if (transport_mode_str != "shm") {
        SPDLOG_ERROR("Invalid transport mode: {}. Use 'shm', 'tcp', or 'hybrid'", transport_mode_str);
        return 1;
    }

    // Parse TCP peers list: "node_id:addr:port,node_id:addr:port,..."
    struct TCPPeerInfo {
        uint32_t node_id;
        std::string addr;
        uint16_t port;
    };
    std::vector<TCPPeerInfo> tcp_peers;
    if (!tcp_peers_str.empty()) {
        std::istringstream peers_stream(tcp_peers_str);
        std::string peer_entry;
        while (std::getline(peers_stream, peer_entry, ',')) {
            // Parse "node_id:addr:port"
            std::istringstream entry_stream(peer_entry);
            std::string token;
            std::vector<std::string> tokens;
            while (std::getline(entry_stream, token, ':')) {
                tokens.push_back(token);
            }
            if (tokens.size() == 3) {
                TCPPeerInfo peer;
                peer.node_id = std::stoul(tokens[0]);
                peer.addr = tokens[1];
                peer.port = std::stoul(tokens[2]);
                tcp_peers.push_back(peer);
            } else {
                SPDLOG_WARN("Invalid TCP peer entry: '{}', expected format node_id:addr:port", peer_entry);
            }
        }
    }

    // Parse communication mode
    CommMode comm_mode = CommMode::TCP;
    if (comm_mode_str == "shm" || comm_mode_str == "shared_memory") {
        comm_mode = CommMode::SHM;
    } else if (comm_mode_str == "pgas-shm" || comm_mode_str == "pgas") {
        comm_mode = CommMode::PGAS_SHM;
    } else if (comm_mode_str == "distributed" || comm_mode_str == "dist") {
        comm_mode = CommMode::DISTRIBUTED;
    } else if (comm_mode_str != "tcp") {
        SPDLOG_ERROR("Invalid communication mode: {}. Use 'tcp', 'shm', 'pgas-shm', or 'distributed'", comm_mode_str);
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
    const char* mode_str = (comm_mode == CommMode::TCP) ? "TCP" :
                           (comm_mode == CommMode::SHM) ? "Shared Memory (/dev/shm)" :
                           (comm_mode == CommMode::PGAS_SHM) ? "PGAS Shared Memory (cxl_backend.h)" :
                           "Distributed Multi-Node";
    SPDLOG_INFO("  Communication Mode: {}", mode_str);
    if (comm_mode == CommMode::PGAS_SHM) {
        SPDLOG_INFO("  PGAS SHM Name: {}", pgas_shm_name);
    }
    if (comm_mode == CommMode::DISTRIBUTED) {
        SPDLOG_INFO("  Node ID: {}", node_id);
        SPDLOG_INFO("  Distributed SHM Name: {}", dist_shm_name);
        const char* transport_str = (transport_mode == DistTransportMode::TCP) ? "TCP" :
                                    (transport_mode == DistTransportMode::HYBRID) ? "Hybrid (SHM+TCP)" : "SHM";
        SPDLOG_INFO("  Transport Mode: {}", transport_str);
        if (transport_mode != DistTransportMode::SHM) {
            SPDLOG_INFO("  TCP Address: {}:{}", tcp_addr, tcp_transport_port);
            if (!tcp_peers.empty()) {
                SPDLOG_INFO("  TCP Peers: {} configured", tcp_peers.size());
                for (const auto& peer : tcp_peers) {
                    SPDLOG_INFO("    Node {}: {}:{}", peer.node_id, peer.addr, peer.port);
                }
            }
        }
        if (!coordinator_shm.empty()) {
            SPDLOG_INFO("  Joining cluster via: {}", coordinator_shm);
        } else {
            SPDLOG_INFO("  Role: Coordinator (creating new cluster)");
        }
    }
    SPDLOG_INFO("  Port: {}", port);
    SPDLOG_INFO("  Topology: {}", topology);
    SPDLOG_INFO("  Capacity: {} MB", capacity);
    SPDLOG_INFO("  Default latency: {} ns", default_latency);
    SPDLOG_INFO("  Interleave size: {} bytes", interleave_size);
    SPDLOG_INFO("CXL Type3 Operations Supported:");
    SPDLOG_INFO("  - CXL_TYPE3_READ");
    SPDLOG_INFO("  - CXL_TYPE3_WRITE");
    if (comm_mode == CommMode::DISTRIBUTED) {
        SPDLOG_INFO("  - Distributed coherency protocol");
        SPDLOG_INFO("  - Inter-node message passing");
        if (transport_mode != DistTransportMode::SHM) {
            SPDLOG_INFO("  - TCP-based remote READ/WRITE");
            SPDLOG_INFO("  - TCP-calibrated LogP model");
            SPDLOG_INFO("  - Distributed MH-SLD coherency via TCP");
        }
    }
    SPDLOG_INFO("========================================");
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Handle distributed mode separately
    if (comm_mode == CommMode::DISTRIBUTED) {
        try {
            SPDLOG_INFO("Starting distributed memory server node {}...", node_id);

            DistributedMemoryServer dist_server(node_id, dist_shm_name, port, capacity, controller,
                                                transport_mode, tcp_addr, tcp_transport_port);

            if (!dist_server.initialize()) {
                SPDLOG_ERROR("Failed to initialize distributed server");
                return 1;
            }

            // Join existing cluster or create new one
            if (!coordinator_shm.empty()) {
                if (!dist_server.join_cluster(coordinator_shm)) {
                    SPDLOG_ERROR("Failed to join cluster");
                    return 1;
                }
            }

            // Connect to TCP peers if transport mode is TCP or HYBRID
            if (transport_mode != DistTransportMode::SHM && !tcp_peers.empty()) {
                SPDLOG_INFO("Connecting to {} TCP peer(s)...", tcp_peers.size());
                for (const auto& peer : tcp_peers) {
                    if (dist_server.connect_tcp_node(peer.node_id, peer.addr, peer.port)) {
                        SPDLOG_INFO("Connected to TCP peer node {} at {}:{}",
                                   peer.node_id, peer.addr, peer.port);
                    } else {
                        SPDLOG_WARN("Failed to connect to TCP peer node {} at {}:{}",
                                   peer.node_id, peer.addr, peer.port);
                    }
                }

                // Run LogP calibration on all connected peers
                SPDLOG_INFO("Running TCP LogP calibration...");
                dist_server.calibrate_tcp_logp();
                SPDLOG_INFO("TCP LogP calibration complete");
            }

            if (!dist_server.start()) {
                SPDLOG_ERROR("Failed to start distributed server");
                return 1;
            }

            SPDLOG_INFO("Distributed node {} is running", node_id);
            SPDLOG_INFO("Press Ctrl+C to stop");

            // Wait for shutdown signal
            while (dist_server.is_running()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));

                // Periodic stats logging
                auto stats = dist_server.get_stats();
                if ((stats.local_reads + stats.local_writes + stats.remote_reads + stats.remote_writes) % 100000 == 0 &&
                    (stats.local_reads + stats.local_writes + stats.remote_reads + stats.remote_writes) > 0) {
                    SPDLOG_INFO("Distributed Stats: local_r={} local_w={} remote_r={} remote_w={} fwd={} coherency={}",
                               stats.local_reads, stats.local_writes,
                               stats.remote_reads, stats.remote_writes,
                               stats.forwarded_requests, stats.coherency_messages);
                }
            }

            dist_server.stop();
            SPDLOG_INFO("Distributed node {} stopped", node_id);

        } catch (const std::exception& e) {
            SPDLOG_ERROR("Distributed server error: {}", e.what());
            return 1;
        }

        return 0;
    }

    try {
        ThreadPerConnectionServer server(port, controller, capacity, backing_file, comm_mode, pgas_shm_name);
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
    // Initialize shared memory for data storage first
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

    // Select communication mode
    switch (comm_mode) {
        case CommMode::SHM:
            SPDLOG_INFO(">>> SHM mode: Skipping TCP socket initialization <<<");
            shm_comm_manager = std::make_unique<ShmCommunicationManager>("/cxlmemsim_comm", true);
            if (!shm_comm_manager->initialize()) {
                SPDLOG_ERROR("Failed to initialize shared memory communication");
                return false;
            }
            SPDLOG_INFO("Server using shared memory communication mode");
            SPDLOG_INFO("No TCP port binding - communication via /dev/shm only");
            return true;

        case CommMode::PGAS_SHM:
            SPDLOG_INFO(">>> PGAS SHM mode: Skipping TCP socket initialization <<<");
            pgas_memory_size_ = shm_info.size;
            if (!init_pgas_shm(pgas_shm_name_, pgas_memory_size_)) {
                SPDLOG_ERROR("Failed to initialize PGAS shared memory");
                return false;
            }
            SPDLOG_INFO("Server using PGAS shared memory mode (cxl_backend.h protocol)");
            SPDLOG_INFO("No TCP port binding - communication via {} only", pgas_shm_name_);
            return true;

        case CommMode::TCP:
            SPDLOG_INFO(">>> TCP mode: Initializing TCP socket <<<");
            break;
    }

    // TCP mode initialization (only reached if comm_mode == TCP)
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

    SPDLOG_INFO("Server listening on TCP port {}", port);
    return true;
}

void ThreadPerConnectionServer::run() {
    switch (comm_mode) {
        case CommMode::SHM:
            SPDLOG_INFO("Running in SHM mode (no TCP accept loop)");
            run_shm_mode();
            return;

        case CommMode::PGAS_SHM:
            SPDLOG_INFO("Running in PGAS SHM mode (no TCP accept loop)");
            run_pgas_shm_mode();
            return;

        case CommMode::TCP:
            SPDLOG_INFO("Running in TCP mode");
            break;
    }

    // TCP accept loop (only reached if comm_mode == TCP)
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

    if (comm_mode == CommMode::PGAS_SHM) {
        cleanup_pgas_shm();
    } else if (comm_mode == CommMode::TCP) {
        close(server_fd);
    }

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
    // CRITICAL: Memory barrier before reading from shared memory
    // This ensures we see all updates from other guests
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Dispatch atomic operations to dedicated handler
    if (req.op_type == OP_ATOMIC_FAA || req.op_type == OP_ATOMIC_CAS || req.op_type == OP_FENCE) {
        handle_atomic_request(thread_id, req, resp);
        return;
    }

    uint64_t cacheline_addr = req.addr & ~(63ULL);  // 64-byte aligned
    bool had_coherency_miss = false;

    // Log CXL Type3 operation with detailed information
    const char* op_name = (req.op_type == OP_READ) ? "CXL_TYPE3_READ" : "CXL_TYPE3_WRITE";
    
    // Log incoming request details
    // SPDLOG_INFO("Thread {}: {} request - addr=0x{:x}, size={}, cacheline=0x{:x}", 
    //             thread_id, op_name, req.addr, req.size, cacheline_addr);
    
    if (req.op_type == OP_WRITE) {
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
    access_elem.push_back(std::make_tuple((uint64_t)req.addr, (uint64_t)req.size));
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
        if (req.op_type == OP_READ) {
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
                // Force sync before reporting failure
                auto* metadata_ptr = shm_manager->get_cacheline_metadata(cacheline_addr);
                if (metadata_ptr) {
                    msync(metadata_ptr, sizeof(CachelineMetadata), MS_INVALIDATE | MS_SYNC);
                }
                SPDLOG_ERROR("Thread {}: Failed to read from shared memory at 0x{:x}",
                            thread_id, (uint64_t)req.addr);
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
            log_periodic_stats("READ", total_reads.load());
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
                            thread_id, (uint64_t)req.addr);
                resp.status = 1;
                congestion_info.active_requests--;
                return;
            }

            // CRITICAL: Force sync to physical memory so other guests can see it
            auto* metadata_ptr = shm_manager->get_cacheline_metadata(cacheline_addr);
            if (metadata_ptr) {
                msync(metadata_ptr, sizeof(CachelineMetadata), MS_SYNC);
            }
            // Force sync the data as well
            void* data_ptr = shm_manager->get_data_area();
            if (data_ptr) {
                msync((uint8_t*)data_ptr + (cacheline_addr & ~CACHELINE_MASK), CACHELINE_SIZE, MS_SYNC);
            }
            std::atomic_thread_fence(std::memory_order_release);

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
            log_periodic_stats("WRITE", total_writes.load());
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
    const char* op_result = (req.op_type == OP_READ) ? "CXL_TYPE3_READ_COMPLETE" : "CXL_TYPE3_WRITE_COMPLETE";
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

void ThreadPerConnectionServer::handle_atomic_request(int thread_id, ServerRequest& req, ServerResponse& resp) {
    uint64_t cacheline_addr = req.addr & ~(63ULL);  // 64-byte aligned

    // Increment active requests for congestion tracking
    congestion_info.active_requests++;

    // Calculate base latency using CXL controller
    std::vector<std::tuple<uint64_t, uint64_t>> access_elem;
    access_elem.push_back(std::make_tuple((uint64_t)req.addr, (uint64_t)sizeof(uint64_t)));
    double base_latency = controller->calculate_latency(access_elem, controller->dramlatency);

    // Atomic operations require exclusive access with proper coherency
    {
        std::unique_lock<std::shared_mutex> lock(memory_mutex);

        // Get metadata from shared memory manager
        auto* metadata = shm_manager->get_cacheline_metadata(cacheline_addr);
        if (!metadata) {
            SPDLOG_ERROR("Thread {}: Failed to get metadata for atomic op at cacheline 0x{:x}",
                        thread_id, cacheline_addr);
            resp.status = 1;
            resp.old_value = 0;
            congestion_info.active_requests--;
            return;
        }

        // Lock the cacheline for this atomic operation
        std::lock_guard<std::mutex> cacheline_lock(metadata->lock);

        // Get pointer to the data location in shared memory
        void* data_area = shm_manager->get_data_area();
        if (!data_area) {
            SPDLOG_ERROR("Thread {}: Failed to get data area for atomic op", thread_id);
            resp.status = 1;
            resp.old_value = 0;
            congestion_info.active_requests--;
            return;
        }

        // Calculate offset within cacheline
        size_t offset = req.addr % 64;
        uint64_t* ptr = reinterpret_cast<uint64_t*>(
            static_cast<uint8_t*>(data_area) + cacheline_addr + offset);

        switch (req.op_type) {
            case OP_ATOMIC_FAA: {
                // Fetch-and-Add: atomically add value and return old value
                uint64_t old_value = __atomic_fetch_add(ptr, req.value, __ATOMIC_SEQ_CST);
                resp.old_value = old_value;

                // Update metadata
                metadata->last_access_time = req.timestamp;
                metadata->state = MODIFIED;
                metadata->owner = thread_id;
                metadata->sharers.clear();
                metadata->version++;

                // Force sync to physical memory
                msync(ptr, sizeof(uint64_t), MS_SYNC);
                __atomic_thread_fence(__ATOMIC_RELEASE);

                total_atomic_faa++;
                log_periodic_stats("ATOMIC_FAA", total_atomic_faa.load());
                SPDLOG_DEBUG("Thread {}: ATOMIC_FAA addr=0x{:x} add={} old={} new={}",
                            thread_id, req.addr, req.value, old_value, old_value + req.value);
                break;
            }

            case OP_ATOMIC_CAS: {
                // Compare-and-Swap: if *ptr == expected, set *ptr = value
                uint64_t expected = req.expected;
                bool success = __atomic_compare_exchange_n(ptr, &expected, req.value,
                                                          false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                resp.old_value = expected;  // Returns actual value (old if success, current if fail)

                // Update metadata
                metadata->last_access_time = req.timestamp;
                if (success) {
                    metadata->state = MODIFIED;
                    metadata->owner = thread_id;
                    metadata->sharers.clear();
                    metadata->version++;
                    total_atomic_cas_success++;

                    // Force sync to physical memory
                    msync(ptr, sizeof(uint64_t), MS_SYNC);
                }
                __atomic_thread_fence(__ATOMIC_RELEASE);

                total_atomic_cas++;
                log_periodic_stats("ATOMIC_CAS", total_atomic_cas.load());
                SPDLOG_DEBUG("Thread {}: ATOMIC_CAS addr=0x{:x} expected={} desired={} actual={} success={}",
                            thread_id, req.addr, req.expected, req.value, expected, success);
                break;
            }

            case OP_FENCE: {
                // Memory fence: ensure all prior operations are visible
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                resp.old_value = 0;

                // Also sync the entire shared memory region
                if (data_area) {
                    auto shm_info = shm_manager->get_shm_info();
                    msync(data_area, shm_info.size, MS_SYNC);
                }

                total_fences++;
                log_periodic_stats("FENCE", total_fences.load());
                SPDLOG_DEBUG("Thread {}: FENCE completed", thread_id);
                break;
            }

            default:
                SPDLOG_ERROR("Thread {}: Unknown atomic op_type: {}", thread_id, req.op_type);
                resp.status = 1;
                resp.old_value = 0;
                congestion_info.active_requests--;
                return;
        }
    }

    // Calculate congestion factor
    double congestion_factor = calculate_congestion_factor();

    // Update congestion statistics
    update_congestion_stats(sizeof(uint64_t));

    // Calculate total latency (atomic ops have similar latency to writes + extra for RMW)
    uint64_t total_latency = calculate_total_latency(base_latency, congestion_factor, true, sizeof(uint64_t));

    // Add extra latency for atomic RMW operations (read-modify-write)
    if (req.op_type != OP_FENCE) {
        total_latency += 20;  // Additional 20ns for atomic RMW overhead
    }

    // Decrement active requests
    congestion_info.active_requests--;

    // Fill response
    resp.status = 0;
    resp.latency_ns = total_latency;
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
        if (req.op_type == OP_GET_SHM_INFO) {
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
        req.value = shm_req.value;       // For atomic FAA/CAS operations
        req.expected = shm_req.expected; // For atomic CAS operation
        std::memcpy(req.data, shm_req.data, sizeof(req.data));

        // Handle special request for shared memory info
        if (req.op_type == OP_GET_SHM_INFO) {
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
        shm_resp.old_value = resp.old_value;  // For atomic operations
        std::memcpy(shm_resp.data, resp.data, sizeof(shm_resp.data));
        
        // Send response back
        shm_comm_manager->send_response(client_id, shm_resp);
    }
}

// ============================================================================
// PGAS Shared Memory Implementation (cxl_backend.h protocol)
// Data and metadata stored together: 64-byte data + 64-byte metadata per cacheline
// ============================================================================

// Memory entry structure (128 bytes total) - matching RDMA server format
struct PGASMemoryEntry {
    // Data portion (64 bytes)
    uint8_t data[64];

    // Metadata portion (64 bytes)
    struct {
        uint8_t cache_state;        // MESI state (0=Invalid, 1=Shared, 2=Exclusive, 3=Modified)
        uint8_t owner_id;           // Current owner host/thread ID
        uint16_t sharers_bitmap;    // Bitmap of hosts/threads sharing this line
        uint32_t access_count;      // Number of accesses
        uint64_t last_access_time;  // Timestamp of last access
        uint64_t virtual_addr;      // Virtual address mapping
        uint64_t physical_addr;     // Physical address
        uint32_t version;           // Version number for coherency
        uint8_t flags;              // Various flags (dirty, locked, etc.)
        uint8_t reserved[23];       // Reserved for future use
    } metadata;
} __attribute__((packed));

bool ThreadPerConnectionServer::init_pgas_shm(const std::string& shm_name, size_t memory_size) {
    // Remove existing shared memory
    shm_unlink(shm_name.c_str());

    // Create shared memory
    pgas_shm_fd_ = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
    if (pgas_shm_fd_ < 0) {
        SPDLOG_ERROR("Failed to create PGAS shared memory {}: {}", shm_name, strerror(errno));
        return false;
    }

    // Calculate total size: header + slots + memory (data + metadata per cacheline)
    // Each cacheline needs 128 bytes (64 data + 64 metadata)
    size_t num_cachelines = memory_size / 64;
    size_t entry_region_size = num_cachelines * sizeof(PGASMemoryEntry);
    size_t header_size = CXL_SHM_HEADER_SIZE(CXL_SHM_MAX_SLOTS);
    size_t total_size = header_size + entry_region_size;

    if (ftruncate(pgas_shm_fd_, total_size) < 0) {
        SPDLOG_ERROR("Failed to set PGAS shared memory size: {}", strerror(errno));
        close(pgas_shm_fd_);
        shm_unlink(shm_name.c_str());
        return false;
    }

    // Map shared memory
    void* mapped = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, pgas_shm_fd_, 0);
    if (mapped == MAP_FAILED) {
        SPDLOG_ERROR("Failed to mmap PGAS shared memory: {}", strerror(errno));
        close(pgas_shm_fd_);
        shm_unlink(shm_name.c_str());
        return false;
    }

    pgas_shm_header_ = (cxl_shm_header_t*)mapped;
    pgas_memory_ = (char*)mapped + header_size;
    pgas_memory_size_ = entry_region_size;

    // Initialize header
    memset(pgas_shm_header_, 0, header_size);
    pgas_shm_header_->magic = CXL_SHM_MAGIC;
    pgas_shm_header_->version = CXL_SHM_VERSION;
    pgas_shm_header_->num_slots = CXL_SHM_MAX_SLOTS;
    pgas_shm_header_->memory_base = 0;
    pgas_shm_header_->memory_size = memory_size;  // Original memory size (data only)
    pgas_shm_header_->num_cachelines = num_cachelines;
    pgas_shm_header_->metadata_enabled = 1;  // Always enabled in PGAS mode
    pgas_shm_header_->entry_size = sizeof(PGASMemoryEntry);
    pgas_shm_header_->flags = CXL_SHM_FLAG_METADATA_ENABLED;

    // Initialize memory entries (data + metadata)
    PGASMemoryEntry* entries = (PGASMemoryEntry*)pgas_memory_;
    for (size_t i = 0; i < num_cachelines; i++) {
        memset(&entries[i], 0, sizeof(PGASMemoryEntry));
        entries[i].metadata.cache_state = CXL_CACHE_INVALID;
        entries[i].metadata.owner_id = 0xFF;  // No owner
        entries[i].metadata.physical_addr = i * 64;  // Cacheline address
    }

    SPDLOG_INFO("PGAS shared memory initialized:");
    SPDLOG_INFO("  Name: {}", shm_name);
    SPDLOG_INFO("  Memory size: {} MB ({} cachelines)", memory_size / (1024*1024), num_cachelines);
    SPDLOG_INFO("  Total mapped size: {} MB (including metadata)", total_size / (1024*1024));
    SPDLOG_INFO("  Entry size: {} bytes (64 data + 64 metadata)", sizeof(PGASMemoryEntry));

    return true;
}

void ThreadPerConnectionServer::run_pgas_shm_mode() {
    SPDLOG_INFO("Running in PGAS shared memory mode");

    // Mark server as ready
    __atomic_store_n(&pgas_shm_header_->server_ready, 1, __ATOMIC_RELEASE);

    // Create worker threads for handling PGAS SHM requests
    const int num_workers = 4;
    std::vector<std::thread> workers;

    for (int i = 0; i < num_workers; i++) {
        workers.emplace_back([this]() {
            while (running) {
                int processed = poll_pgas_shm_requests();
                if (processed == 0) {
                    // No requests - sleep briefly to reduce CPU usage
                    usleep(100);  // 100us
                }
            }
        });
    }

    // Wait for workers to finish
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

int ThreadPerConnectionServer::poll_pgas_shm_requests() {
    if (!pgas_shm_header_ || !running) return 0;

    int processed = 0;
    PGASMemoryEntry* entries = (PGASMemoryEntry*)pgas_memory_;
    size_t num_cachelines = pgas_shm_header_->memory_size / 64;

    for (uint32_t i = 0; i < pgas_shm_header_->num_slots; i++) {
        cxl_shm_slot_t* slot = &pgas_shm_header_->slots[i];

        uint32_t req = __atomic_load_n(&slot->req_type, __ATOMIC_ACQUIRE);
        if (req == CXL_SHM_REQ_NONE) continue;

        // Calculate cacheline index from address
        uint64_t addr = slot->addr;
        size_t cacheline_idx = addr / 64;

        if (cacheline_idx >= num_cachelines) {
            slot->resp_status = CXL_SHM_RESP_ERROR;
            __atomic_store_n(&slot->req_type, CXL_SHM_REQ_NONE, __ATOMIC_RELEASE);
            continue;
        }

        PGASMemoryEntry* entry = &entries[cacheline_idx];

        // Calculate latency using CXL controller
        std::vector<std::tuple<uint64_t, uint64_t>> access_elem;
        access_elem.push_back(std::make_tuple(addr, slot->size));
        double base_latency = controller->calculate_latency(access_elem, controller->dramlatency);

        switch (req) {
            case CXL_SHM_REQ_READ: {
                // Update metadata
                entry->metadata.access_count++;
                entry->metadata.last_access_time = slot->timestamp;

                // Copy data to slot
                size_t copy_size = std::min((size_t)slot->size, (size_t)64);
                size_t offset = addr % 64;
                memcpy((void*)slot->data, entry->data + offset, copy_size);

                // Copy metadata to response (use latency_ns field for cache_state)
                slot->latency_ns = (uint64_t)base_latency;

                // Store cache state in first byte of data response padding
                // Client can check this for coherency information

                __atomic_thread_fence(__ATOMIC_RELEASE);
                slot->resp_status = CXL_SHM_RESP_OK;
                total_reads++;
                log_periodic_stats("PGAS_READ", total_reads.load());
                processed++;
                break;
            }

            case CXL_SHM_REQ_WRITE: {
                // Copy data from slot to memory
                size_t copy_size = std::min((size_t)slot->size, (size_t)64);
                size_t offset = addr % 64;
                memcpy(entry->data + offset, (void*)slot->data, copy_size);

                // Update metadata
                entry->metadata.access_count++;
                entry->metadata.last_access_time = slot->timestamp;
                entry->metadata.cache_state = 3;  // MODIFIED
                entry->metadata.version++;

                slot->latency_ns = (uint64_t)base_latency;
                __atomic_thread_fence(__ATOMIC_RELEASE);
                slot->resp_status = CXL_SHM_RESP_OK;
                total_writes++;
                log_periodic_stats("PGAS_WRITE", total_writes.load());
                processed++;
                break;
            }

            case CXL_SHM_REQ_ATOMIC_FAA: {
                if (slot->addr + sizeof(uint64_t) <= num_cachelines * 64) {
                    uint64_t* ptr = (uint64_t*)(entry->data + (addr % 64));
                    uint64_t old = __atomic_fetch_add(ptr, slot->value, __ATOMIC_SEQ_CST);
                    memcpy((void*)slot->data, &old, sizeof(old));

                    entry->metadata.access_count++;
                    entry->metadata.cache_state = 3;  // MODIFIED
                    entry->metadata.version++;

                    slot->latency_ns = (uint64_t)base_latency;
                    __atomic_thread_fence(__ATOMIC_RELEASE);
                    slot->resp_status = CXL_SHM_RESP_OK;
                    total_atomic_faa++;
                    log_periodic_stats("PGAS_FAA", total_atomic_faa.load());
                } else {
                    slot->resp_status = CXL_SHM_RESP_ERROR;
                }
                processed++;
                break;
            }

            case CXL_SHM_REQ_ATOMIC_CAS: {
                if (slot->addr + sizeof(uint64_t) <= num_cachelines * 64) {
                    uint64_t* ptr = (uint64_t*)(entry->data + (addr % 64));
                    uint64_t expected = slot->expected;
                    __atomic_compare_exchange_n(ptr, &expected, slot->value,
                                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                    memcpy((void*)slot->data, &expected, sizeof(expected));

                    entry->metadata.access_count++;
                    entry->metadata.version++;

                    slot->latency_ns = (uint64_t)base_latency;
                    __atomic_thread_fence(__ATOMIC_RELEASE);
                    slot->resp_status = CXL_SHM_RESP_OK;
                    total_atomic_cas++;
                    log_periodic_stats("PGAS_CAS", total_atomic_cas.load());
                } else {
                    slot->resp_status = CXL_SHM_RESP_ERROR;
                }
                processed++;
                break;
            }

            case CXL_SHM_REQ_FENCE: {
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                slot->latency_ns = 0;
                __atomic_thread_fence(__ATOMIC_RELEASE);
                slot->resp_status = CXL_SHM_RESP_OK;
                total_fences++;
                log_periodic_stats("PGAS_FENCE", total_fences.load());
                processed++;
                break;
            }

            default:
                break;
        }

        // Clear request to mark slot as free
        __atomic_store_n(&slot->req_type, CXL_SHM_REQ_NONE, __ATOMIC_RELEASE);
    }

    return processed;
}

void ThreadPerConnectionServer::cleanup_pgas_shm() {
    if (pgas_shm_header_) {
        __atomic_store_n(&pgas_shm_header_->server_ready, 0, __ATOMIC_RELEASE);

        // Calculate total size for munmap
        size_t header_size = CXL_SHM_HEADER_SIZE(pgas_shm_header_->num_slots);
        size_t total_size = header_size + pgas_memory_size_;

        munmap(pgas_shm_header_, total_size);
        pgas_shm_header_ = nullptr;
        pgas_memory_ = nullptr;
    }

    if (pgas_shm_fd_ >= 0) {
        close(pgas_shm_fd_);
        pgas_shm_fd_ = -1;
    }

    shm_unlink(pgas_shm_name_.c_str());
    SPDLOG_INFO("PGAS shared memory cleaned up");
}
