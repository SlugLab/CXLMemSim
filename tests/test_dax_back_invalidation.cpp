/*
 * DAX Back Invalidation Test for CXLMemSim
 * Tests MESI coherency using DAX device with TCP backend
 *
 * Usage:
 *   Node 1: ./test_dax_back_invalidation --mode writer --dax /dev/dax0.0 --server 192.168.100.10:9999
 *   Node 2: ./test_dax_back_invalidation --mode reader --dax /dev/dax0.0 --server 192.168.100.10:9999
 */

#include <iostream>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

// Protocol constants
constexpr uint8_t OP_READ = 0;
constexpr uint8_t OP_WRITE = 1;
constexpr uint8_t OP_FENCE = 5;

// DAX device size to map (64MB)
constexpr size_t DAX_MAP_SIZE = 64 * 1024 * 1024;

// Test addresses (offsets into DAX region)
constexpr uint64_t TEST_OFFSET = 0x1000;
constexpr size_t CACHELINE_SIZE = 64;

#pragma pack(push, 1)
struct ServerRequest {
    uint8_t op_type;
    uint64_t addr;
    uint64_t size;
    uint64_t timestamp;
    uint64_t value;
    uint64_t expected;
    uint8_t data[64];
};

struct ServerResponse {
    uint8_t status;
    uint64_t latency_ns;
    uint64_t old_value;
    uint8_t data[64];
};
#pragma pack(pop)

class DAXCXLClient {
private:
    int sock_fd = -1;
    int dax_fd = -1;
    void* dax_mem = nullptr;
    size_t dax_size = 0;
    std::string server_addr;
    int server_port;
    std::string dax_path;
    uint64_t node_id;

public:
    DAXCXLClient(const std::string& dax, const std::string& addr, int port)
        : dax_path(dax), server_addr(addr), server_port(port) {
        node_id = getpid();
    }

    ~DAXCXLClient() {
        if (dax_mem && dax_mem != MAP_FAILED) {
            munmap(dax_mem, dax_size);
        }
        if (dax_fd >= 0) close(dax_fd);
        if (sock_fd >= 0) close(sock_fd);
    }

    bool connect() {
        // Connect to CXLMemSim server via TCP
        sock_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            std::cerr << "Failed to create socket" << std::endl;
            return false;
        }

        struct sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(server_port);

        if (inet_pton(AF_INET, server_addr.c_str(), &serv_addr.sin_addr) <= 0) {
            std::cerr << "Invalid server address: " << server_addr << std::endl;
            return false;
        }

        if (::connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            std::cerr << "TCP connection failed to " << server_addr << ":" << server_port << std::endl;
            return false;
        }
        std::cout << "TCP connected to CXLMemSim server at " << server_addr << ":" << server_port << std::endl;

        // Open and map DAX device
        dax_fd = open(dax_path.c_str(), O_RDWR);
        if (dax_fd < 0) {
            std::cerr << "Failed to open DAX device: " << dax_path << " - " << strerror(errno) << std::endl;
            return false;
        }

        // Get DAX device size
        struct stat st;
        if (fstat(dax_fd, &st) < 0) {
            // For DAX devices, we might need to use a different method
            dax_size = DAX_MAP_SIZE;
        } else {
            dax_size = st.st_size > 0 ? st.st_size : DAX_MAP_SIZE;
        }

        // Map DAX device
        dax_mem = mmap(nullptr, dax_size, PROT_READ | PROT_WRITE, MAP_SHARED, dax_fd, 0);
        if (dax_mem == MAP_FAILED) {
            std::cerr << "Failed to mmap DAX device: " << strerror(errno) << std::endl;
            dax_mem = nullptr;
            return false;
        }

        std::cout << "DAX device mapped: " << dax_path << " (" << (dax_size / 1024 / 1024) << " MB)" << std::endl;
        return true;
    }

    // Notify server about read (for coherency tracking)
    bool notify_read(uint64_t offset, size_t size, uint64_t* latency = nullptr) {
        ServerRequest req{};
        req.op_type = OP_READ;
        req.addr = offset;
        req.size = size;
        req.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();

        if (send(sock_fd, &req, sizeof(req), 0) != sizeof(req)) {
            std::cerr << "Failed to send read notification" << std::endl;
            return false;
        }

        ServerResponse resp{};
        if (recv(sock_fd, &resp, sizeof(resp), MSG_WAITALL) != sizeof(resp)) {
            std::cerr << "Failed to receive read response" << std::endl;
            return false;
        }

        if (latency) *latency = resp.latency_ns;
        return resp.status == 0;
    }

    // Notify server about write (for coherency - triggers back invalidation)
    bool notify_write(uint64_t offset, const void* data, size_t size, uint64_t* latency = nullptr) {
        ServerRequest req{};
        req.op_type = OP_WRITE;
        req.addr = offset;
        req.size = size;
        req.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        memcpy(req.data, data, std::min(size, sizeof(req.data)));

        if (send(sock_fd, &req, sizeof(req), 0) != sizeof(req)) {
            std::cerr << "Failed to send write notification" << std::endl;
            return false;
        }

        ServerResponse resp{};
        if (recv(sock_fd, &resp, sizeof(resp), MSG_WAITALL) != sizeof(resp)) {
            std::cerr << "Failed to receive write response" << std::endl;
            return false;
        }

        if (latency) *latency = resp.latency_ns;
        return resp.status == 0;
    }

    // Issue fence
    bool fence() {
        ServerRequest req{};
        req.op_type = OP_FENCE;
        req.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();

        if (send(sock_fd, &req, sizeof(req), 0) != sizeof(req)) {
            return false;
        }

        ServerResponse resp{};
        return recv(sock_fd, &resp, sizeof(resp), MSG_WAITALL) == sizeof(resp);
    }

    // Direct DAX read (local memory access)
    void dax_read(uint64_t offset, void* data, size_t size) {
        if (dax_mem && offset + size <= dax_size) {
            void* addr = (uint8_t*)dax_mem + offset;
            // Memory barrier before read
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            memcpy(data, addr, size);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
        }
    }

    // Direct DAX write (local memory access)
    void dax_write(uint64_t offset, const void* data, size_t size) {
        if (dax_mem && offset + size <= dax_size) {
            void* addr = (uint8_t*)dax_mem + offset;
            memcpy(addr, data, size);
            // Memory barrier after write
            __atomic_thread_fence(__ATOMIC_RELEASE);
            // Sync to ensure visibility
            msync(addr, size, MS_SYNC);
        }
    }

    // Combined read: DAX access + server notification for coherency
    bool read_with_coherency(uint64_t offset, void* data, size_t size, uint64_t* latency = nullptr) {
        // First notify server (for coherency tracking)
        if (!notify_read(offset, size, latency)) {
            return false;
        }
        // Then read from DAX
        dax_read(offset, data, size);
        return true;
    }

    // Combined write: DAX access + server notification (triggers back invalidation)
    bool write_with_coherency(uint64_t offset, const void* data, size_t size, uint64_t* latency = nullptr) {
        // Write to DAX first
        dax_write(offset, data, size);
        // Then notify server (triggers back invalidation to other nodes)
        return notify_write(offset, data, size, latency);
    }

    void* get_dax_mem() { return dax_mem; }
    size_t get_dax_size() { return dax_size; }
};

void print_data(const char* label, const uint8_t* data, size_t size) {
    std::cout << label << ": ";
    for (size_t i = 0; i < std::min(size, (size_t)16); i++) {
        printf("%02x ", data[i]);
    }
    std::cout << "\"";
    for (size_t i = 0; i < std::min(size, (size_t)32); i++) {
        char c = data[i];
        std::cout << (isprint(c) ? c : '.');
    }
    std::cout << "\"" << std::endl;
}

void run_writer_test(DAXCXLClient& client, int iterations) {
    std::cout << "\n=== DAX WRITER MODE ===" << std::endl;
    std::cout << "This node writes to DAX memory and notifies server for back invalidation" << std::endl;
    std::cout << "Press Enter to start..." << std::endl;
    std::cin.get();

    uint8_t write_data[CACHELINE_SIZE];
    uint64_t latency;

    for (int i = 0; i < iterations; i++) {
        memset(write_data, 0, sizeof(write_data));
        snprintf((char*)write_data, sizeof(write_data), "DAX Write #%d from node", i);

        std::cout << "\n--- Iteration " << i << " ---" << std::endl;

        auto start = std::chrono::steady_clock::now();

        // Write to DAX and notify server
        if (client.write_with_coherency(TEST_OFFSET, write_data, CACHELINE_SIZE, &latency)) {
            auto end = std::chrono::steady_clock::now();
            auto local_time = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

            std::cout << "WRITE to DAX offset 0x" << std::hex << TEST_OFFSET << std::dec << std::endl;
            std::cout << "  Server latency: " << latency << " ns" << std::endl;
            std::cout << "  Total time: " << local_time << " ns" << std::endl;
            print_data("  Data", write_data, CACHELINE_SIZE);
        } else {
            std::cerr << "WRITE failed!" << std::endl;
        }

        client.fence();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "\n=== Writer complete ===" << std::endl;
}

void run_reader_test(DAXCXLClient& client, int iterations) {
    std::cout << "\n=== DAX READER MODE ===" << std::endl;
    std::cout << "This node reads from DAX memory and detects changes via coherency" << std::endl;

    uint8_t read_data[CACHELINE_SIZE];
    uint8_t last_data[CACHELINE_SIZE] = {0};
    uint64_t latency;

    // Initial read
    std::cout << "\nInitial read..." << std::endl;
    if (client.read_with_coherency(TEST_OFFSET, read_data, CACHELINE_SIZE, &latency)) {
        std::cout << "Initial READ - server latency: " << latency << " ns" << std::endl;
        print_data("Initial data", read_data, CACHELINE_SIZE);
        memcpy(last_data, read_data, sizeof(last_data));
    }

    std::cout << "\nPolling for changes (start writer on other node)..." << std::endl;

    int change_count = 0;
    int poll_count = 0;
    int max_polls = iterations * 10;

    while (poll_count < max_polls) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        poll_count++;

        if (client.read_with_coherency(TEST_OFFSET, read_data, CACHELINE_SIZE, &latency)) {
            if (memcmp(read_data, last_data, CACHELINE_SIZE) != 0) {
                change_count++;
                std::cout << "\n*** CHANGE DETECTED #" << change_count << " (poll " << poll_count << ") ***" << std::endl;
                std::cout << "  Server latency: " << latency << " ns" << std::endl;
                print_data("  Old", last_data, CACHELINE_SIZE);
                print_data("  New", read_data, CACHELINE_SIZE);
                memcpy(last_data, read_data, sizeof(last_data));
            } else {
                std::cout << "." << std::flush;
            }
        }
    }

    std::cout << "\n\n=== Reader complete ===" << std::endl;
    std::cout << "Changes detected: " << change_count << " in " << poll_count << " polls" << std::endl;
}

void run_pingpong_test(DAXCXLClient& client, bool is_initiator, int rounds) {
    std::cout << "\n=== PING-PONG TEST ===" << std::endl;
    std::cout << "Role: " << (is_initiator ? "INITIATOR" : "RESPONDER") << std::endl;

    uint8_t data[CACHELINE_SIZE];
    uint64_t latency;
    int my_turn = is_initiator ? 0 : 1;

    // Sync point
    std::cout << "Press Enter when both nodes are ready..." << std::endl;
    std::cin.get();

    auto start = std::chrono::steady_clock::now();
    uint64_t total_latency = 0;

    for (int round = 0; round < rounds; round++) {
        if ((round % 2) == my_turn) {
            // My turn to write
            snprintf((char*)data, sizeof(data), "Round %d from %s",
                    round, is_initiator ? "initiator" : "responder");

            if (client.write_with_coherency(TEST_OFFSET, data, CACHELINE_SIZE, &latency)) {
                total_latency += latency;
                if (round < 5 || round == rounds - 1) {
                    std::cout << "Round " << round << ": WRITE, latency=" << latency << "ns" << std::endl;
                }
            }
            client.fence();
        } else {
            // Wait for other node
            int retries = 100;
            bool got_update = false;
            uint8_t expected_round = round;

            while (retries-- > 0 && !got_update) {
                if (client.read_with_coherency(TEST_OFFSET, data, CACHELINE_SIZE, &latency)) {
                    // Check if data contains expected round
                    char expected[32];
                    snprintf(expected, sizeof(expected), "Round %d", round);
                    if (strstr((char*)data, expected) != nullptr) {
                        got_update = true;
                        total_latency += latency;
                        if (round < 5 || round == rounds - 1) {
                            std::cout << "Round " << round << ": READ, latency=" << latency << "ns" << std::endl;
                        }
                    }
                }
                if (!got_update) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            if (!got_update) {
                std::cerr << "Timeout waiting for round " << round << std::endl;
            }
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "\n=== Ping-Pong Results ===" << std::endl;
    std::cout << "Rounds: " << rounds << std::endl;
    std::cout << "Total time: " << duration << " ms" << std::endl;
    std::cout << "Avg latency: " << (total_latency / rounds) << " ns" << std::endl;
    std::cout << "Round-trip rate: " << (rounds * 1000.0 / duration) << " ops/sec" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string mode = "reader";
    std::string server = "192.168.100.10:9999";
    std::string dax_path = "/dev/dax0.0";
    int iterations = 10;
    bool pingpong = false;
    bool initiator = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--server" && i + 1 < argc) {
            server = argv[++i];
        } else if (arg == "--dax" && i + 1 < argc) {
            dax_path = argv[++i];
        } else if (arg == "--iterations" && i + 1 < argc) {
            iterations = std::stoi(argv[++i]);
        } else if (arg == "--pingpong") {
            pingpong = true;
        } else if (arg == "--initiator") {
            initiator = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --mode <reader|writer>  Test mode" << std::endl;
            std::cout << "  --server <host:port>    CXLMemSim server" << std::endl;
            std::cout << "  --dax <path>            DAX device path" << std::endl;
            std::cout << "  --iterations <n>        Number of iterations" << std::endl;
            std::cout << "  --pingpong              Run ping-pong test" << std::endl;
            std::cout << "  --initiator             Be the initiator in ping-pong" << std::endl;
            return 0;
        }
    }

    std::cout << "=== DAX Back Invalidation Test ===" << std::endl;
    std::cout << "DAX device: " << dax_path << std::endl;

    size_t colon = server.find(':');
    std::string host = server.substr(0, colon);
    int port = std::stoi(server.substr(colon + 1));
    std::cout << "Server: " << host << ":" << port << std::endl;

    DAXCXLClient client(dax_path, host, port);
    if (!client.connect()) {
        std::cerr << "Failed to connect!" << std::endl;
        return 1;
    }

    if (pingpong) {
        run_pingpong_test(client, initiator, iterations);
    } else if (mode == "writer") {
        run_writer_test(client, iterations);
    } else {
        run_reader_test(client, iterations);
    }

    return 0;
}
