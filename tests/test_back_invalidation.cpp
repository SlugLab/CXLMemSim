/*
 * Back Invalidation Test for CXLMemSim
 * Tests MESI coherency protocol across multiple nodes/threads
 *
 * Usage:
 *   Node 1 (writer): ./test_back_invalidation --mode writer --server 192.168.100.10:9999
 *   Node 2 (reader): ./test_back_invalidation --mode reader --server 192.168.100.10:9999
 *
 * Or for PGAS SHM mode:
 *   ./test_back_invalidation --mode writer --shm /cxlmemsim_pgas
 *   ./test_back_invalidation --mode reader --shm /cxlmemsim_pgas
 */

#include <iostream>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

// Protocol constants (matching server)
constexpr uint8_t OP_READ = 0;
constexpr uint8_t OP_WRITE = 1;
constexpr uint8_t OP_ATOMIC_FAA = 3;
constexpr uint8_t OP_ATOMIC_CAS = 4;
constexpr uint8_t OP_FENCE = 5;

// Test address (must be within CXL memory range)
constexpr uint64_t TEST_ADDR = 0x1000;
constexpr uint64_t TEST_SIZE = 64;

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

// PGAS SHM structures (matching cxl_backend.h)
#define CXL_SHM_MAGIC 0x43584C53484D454DULL
#define CXL_SHM_MAX_SLOTS 64

#define CXL_SHM_REQ_NONE 0
#define CXL_SHM_REQ_READ 1
#define CXL_SHM_REQ_WRITE 2
#define CXL_SHM_REQ_FENCE 5

#define CXL_SHM_RESP_NONE 0
#define CXL_SHM_RESP_OK 1

struct CXLCachelineMetadata {
    uint8_t cache_state;
    uint8_t owner_id;
    uint16_t sharers_bitmap;
    uint32_t access_count;
    uint64_t last_access_time;
    uint64_t virtual_addr;
    uint64_t physical_addr;
    uint32_t version;
    uint8_t flags;
    uint8_t reserved[23];
} __attribute__((packed));

struct CXLShmSlot {
    volatile uint32_t req_type;
    volatile uint32_t resp_status;
    volatile uint64_t addr;
    volatile uint64_t size;
    volatile uint64_t value;
    volatile uint64_t expected;
    volatile uint64_t latency_ns;
    volatile uint64_t timestamp;
    uint8_t data[64];
    CXLCachelineMetadata metadata;
} __attribute__((aligned(256)));

struct CXLShmHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t num_slots;
    volatile uint32_t server_ready;
    uint32_t flags;
    uint64_t memory_base;
    uint64_t memory_size;
    uint64_t num_cachelines;
    uint32_t metadata_enabled;
    uint32_t entry_size;
    uint8_t padding[64 - 56];
    CXLShmSlot slots[];
} __attribute__((aligned(64)));

class CXLClient {
public:
    enum Mode { TCP, SHM };

private:
    Mode mode;
    int sock_fd = -1;
    int shm_fd = -1;
    CXLShmHeader* shm_header = nullptr;
    int slot_id = 0;
    std::string server_addr;
    int server_port;
    std::string shm_name;

public:
    CXLClient(const std::string& addr, int port)
        : mode(TCP), server_addr(addr), server_port(port) {}

    CXLClient(const std::string& shm)
        : mode(SHM), shm_name(shm) {}

    bool connect() {
        if (mode == TCP) {
            return connect_tcp();
        } else {
            return connect_shm();
        }
    }

    bool connect_tcp() {
        sock_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            std::cerr << "Failed to create socket" << std::endl;
            return false;
        }

        struct sockaddr_in serv_addr{};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(server_port);

        if (inet_pton(AF_INET, server_addr.c_str(), &serv_addr.sin_addr) <= 0) {
            std::cerr << "Invalid address" << std::endl;
            return false;
        }

        if (::connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            std::cerr << "Connection failed to " << server_addr << ":" << server_port << std::endl;
            return false;
        }

        std::cout << "Connected to " << server_addr << ":" << server_port << std::endl;
        return true;
    }

    bool connect_shm() {
        shm_fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
        if (shm_fd < 0) {
            std::cerr << "Failed to open SHM: " << shm_name << std::endl;
            return false;
        }

        struct stat sb;
        if (fstat(shm_fd, &sb) < 0) {
            std::cerr << "fstat failed" << std::endl;
            return false;
        }

        void* mapped = mmap(nullptr, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (mapped == MAP_FAILED) {
            std::cerr << "mmap failed" << std::endl;
            return false;
        }

        shm_header = (CXLShmHeader*)mapped;
        if (shm_header->magic != CXL_SHM_MAGIC) {
            std::cerr << "Invalid SHM magic" << std::endl;
            return false;
        }

        // Wait for server ready
        int retries = 100;
        while (!__atomic_load_n(&shm_header->server_ready, __ATOMIC_ACQUIRE) && retries > 0) {
            usleep(10000);
            retries--;
        }
        if (retries == 0) {
            std::cerr << "Server not ready" << std::endl;
            return false;
        }

        slot_id = getpid() % shm_header->num_slots;
        std::cout << "Connected to SHM: " << shm_name << " (slot " << slot_id << ")" << std::endl;
        return true;
    }

    bool read(uint64_t addr, uint8_t* data, size_t size, uint64_t* latency = nullptr) {
        if (mode == TCP) {
            return read_tcp(addr, data, size, latency);
        } else {
            return read_shm(addr, data, size, latency);
        }
    }

    bool write(uint64_t addr, const uint8_t* data, size_t size, uint64_t* latency = nullptr) {
        if (mode == TCP) {
            return write_tcp(addr, data, size, latency);
        } else {
            return write_shm(addr, data, size, latency);
        }
    }

    bool fence() {
        if (mode == TCP) {
            return fence_tcp();
        } else {
            return fence_shm();
        }
    }

private:
    bool read_tcp(uint64_t addr, uint8_t* data, size_t size, uint64_t* latency) {
        ServerRequest req{};
        req.op_type = OP_READ;
        req.addr = addr;
        req.size = size;
        req.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();

        if (send(sock_fd, &req, sizeof(req), 0) != sizeof(req)) {
            return false;
        }

        ServerResponse resp{};
        if (recv(sock_fd, &resp, sizeof(resp), MSG_WAITALL) != sizeof(resp)) {
            return false;
        }

        if (resp.status == 0) {
            memcpy(data, resp.data, std::min(size, sizeof(resp.data)));
            if (latency) *latency = resp.latency_ns;
            return true;
        }
        return false;
    }

    bool write_tcp(uint64_t addr, const uint8_t* data, size_t size, uint64_t* latency) {
        ServerRequest req{};
        req.op_type = OP_WRITE;
        req.addr = addr;
        req.size = size;
        req.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        memcpy(req.data, data, std::min(size, sizeof(req.data)));

        if (send(sock_fd, &req, sizeof(req), 0) != sizeof(req)) {
            return false;
        }

        ServerResponse resp{};
        if (recv(sock_fd, &resp, sizeof(resp), MSG_WAITALL) != sizeof(resp)) {
            return false;
        }

        if (latency) *latency = resp.latency_ns;
        return resp.status == 0;
    }

    bool fence_tcp() {
        ServerRequest req{};
        req.op_type = OP_FENCE;
        req.timestamp = std::chrono::steady_clock::now().time_since_epoch().count();

        if (send(sock_fd, &req, sizeof(req), 0) != sizeof(req)) {
            return false;
        }

        ServerResponse resp{};
        return recv(sock_fd, &resp, sizeof(resp), MSG_WAITALL) == sizeof(resp);
    }

    bool read_shm(uint64_t addr, uint8_t* data, size_t size, uint64_t* latency) {
        CXLShmSlot* slot = &shm_header->slots[slot_id];

        // Wait for slot to be free
        int retries = 1000;
        while (__atomic_load_n(&slot->req_type, __ATOMIC_ACQUIRE) != CXL_SHM_REQ_NONE && retries > 0) {
            usleep(100);
            retries--;
        }
        if (retries == 0) return false;

        slot->addr = addr;
        slot->size = size;
        slot->timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&slot->req_type, CXL_SHM_REQ_READ, __ATOMIC_RELEASE);

        // Wait for response
        retries = 10000;
        while (__atomic_load_n(&slot->resp_status, __ATOMIC_ACQUIRE) == CXL_SHM_RESP_NONE && retries > 0) {
            usleep(10);
            retries--;
        }
        if (retries == 0) return false;

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        bool success = slot->resp_status == CXL_SHM_RESP_OK;
        if (success) {
            memcpy(data, (void*)slot->data, std::min(size, (size_t)64));
            if (latency) *latency = slot->latency_ns;
        }

        __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
        __atomic_store_n(&slot->req_type, CXL_SHM_REQ_NONE, __ATOMIC_RELEASE);
        return success;
    }

    bool write_shm(uint64_t addr, const uint8_t* data, size_t size, uint64_t* latency) {
        CXLShmSlot* slot = &shm_header->slots[slot_id];

        int retries = 1000;
        while (__atomic_load_n(&slot->req_type, __ATOMIC_ACQUIRE) != CXL_SHM_REQ_NONE && retries > 0) {
            usleep(100);
            retries--;
        }
        if (retries == 0) return false;

        slot->addr = addr;
        slot->size = size;
        slot->timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        memcpy((void*)slot->data, data, std::min(size, (size_t)64));
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&slot->req_type, CXL_SHM_REQ_WRITE, __ATOMIC_RELEASE);

        retries = 10000;
        while (__atomic_load_n(&slot->resp_status, __ATOMIC_ACQUIRE) == CXL_SHM_RESP_NONE && retries > 0) {
            usleep(10);
            retries--;
        }
        if (retries == 0) return false;

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        bool success = slot->resp_status == CXL_SHM_RESP_OK;
        if (latency) *latency = slot->latency_ns;

        __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
        __atomic_store_n(&slot->req_type, CXL_SHM_REQ_NONE, __ATOMIC_RELEASE);
        return success;
    }

    bool fence_shm() {
        CXLShmSlot* slot = &shm_header->slots[slot_id];

        int retries = 1000;
        while (__atomic_load_n(&slot->req_type, __ATOMIC_ACQUIRE) != CXL_SHM_REQ_NONE && retries > 0) {
            usleep(100);
            retries--;
        }
        if (retries == 0) return false;

        __atomic_store_n(&slot->req_type, CXL_SHM_REQ_FENCE, __ATOMIC_RELEASE);

        retries = 10000;
        while (__atomic_load_n(&slot->resp_status, __ATOMIC_ACQUIRE) == CXL_SHM_RESP_NONE && retries > 0) {
            usleep(10);
            retries--;
        }

        __atomic_store_n(&slot->resp_status, CXL_SHM_RESP_NONE, __ATOMIC_RELEASE);
        __atomic_store_n(&slot->req_type, CXL_SHM_REQ_NONE, __ATOMIC_RELEASE);
        return retries > 0;
    }
};

void print_data(const char* label, const uint8_t* data, size_t size) {
    std::cout << label << ": ";
    for (size_t i = 0; i < std::min(size, (size_t)16); i++) {
        printf("%02x ", data[i]);
    }
    std::cout << std::endl;
}

void run_writer_test(CXLClient& client, int iterations) {
    std::cout << "\n=== WRITER MODE ===" << std::endl;
    std::cout << "This node will write to cachelines to trigger back invalidations" << std::endl;
    std::cout << "Start the reader on another node first!" << std::endl;
    std::cout << "Press Enter to start writing..." << std::endl;
    std::cin.get();

    uint8_t write_data[64];
    uint64_t latency;

    for (int i = 0; i < iterations; i++) {
        // Prepare data with iteration number
        memset(write_data, 0, sizeof(write_data));
        snprintf((char*)write_data, sizeof(write_data), "Write iteration %d from writer", i);

        std::cout << "\n--- Iteration " << i << " ---" << std::endl;

        // Write to the shared address
        if (client.write(TEST_ADDR, write_data, TEST_SIZE, &latency)) {
            std::cout << "WRITE to 0x" << std::hex << TEST_ADDR << std::dec
                      << " - latency: " << latency << " ns" << std::endl;
            print_data("Data written", write_data, 16);
        } else {
            std::cerr << "WRITE failed!" << std::endl;
        }

        // Issue fence to ensure visibility
        client.fence();
        std::cout << "FENCE issued" << std::endl;

        // Wait a bit for reader to see it
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "\n=== Writer test complete ===" << std::endl;
}

void run_reader_test(CXLClient& client, int iterations) {
    std::cout << "\n=== READER MODE ===" << std::endl;
    std::cout << "This node will read cachelines and detect back invalidations" << std::endl;

    uint8_t read_data[64];
    uint64_t latency;
    uint8_t last_data[64] = {0};

    // Initial read to get cacheline in SHARED state
    std::cout << "\nInitial read to establish shared state..." << std::endl;
    if (client.read(TEST_ADDR, read_data, TEST_SIZE, &latency)) {
        std::cout << "Initial READ - latency: " << latency << " ns" << std::endl;
        print_data("Initial data", read_data, 16);
        memcpy(last_data, read_data, sizeof(last_data));
    }

    std::cout << "\nWaiting for writer to modify data..." << std::endl;
    std::cout << "(Start the writer on another node now)" << std::endl;

    int invalidation_count = 0;
    for (int i = 0; i < iterations * 2; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Read again - should detect changes from writer
        if (client.read(TEST_ADDR, read_data, TEST_SIZE, &latency)) {
            // Check if data changed (indicates back invalidation worked)
            if (memcmp(read_data, last_data, sizeof(read_data)) != 0) {
                invalidation_count++;
                std::cout << "\n*** BACK INVALIDATION DETECTED (#" << invalidation_count << ") ***" << std::endl;
                std::cout << "READ latency: " << latency << " ns (higher = coherency miss)" << std::endl;
                print_data("Old data", last_data, 16);
                print_data("New data", read_data, 16);
                memcpy(last_data, read_data, sizeof(last_data));
            } else {
                std::cout << "." << std::flush;
            }
        }
    }

    std::cout << "\n\n=== Reader test complete ===" << std::endl;
    std::cout << "Total back invalidations detected: " << invalidation_count << std::endl;
}

void run_stress_test(CXLClient& client, int num_ops) {
    std::cout << "\n=== STRESS TEST ===" << std::endl;
    std::cout << "Running " << num_ops << " mixed read/write operations..." << std::endl;

    uint8_t data[64];
    uint64_t total_read_latency = 0, total_write_latency = 0;
    int reads = 0, writes = 0;
    uint64_t latency;

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < num_ops; i++) {
        uint64_t addr = TEST_ADDR + (i % 16) * 64;  // Access 16 different cachelines

        if (i % 2 == 0) {
            // Read
            if (client.read(addr, data, 64, &latency)) {
                total_read_latency += latency;
                reads++;
            }
        } else {
            // Write
            memset(data, i & 0xFF, 64);
            if (client.write(addr, data, 64, &latency)) {
                total_write_latency += latency;
                writes++;
            }
        }

        if ((i + 1) % 1000 == 0) {
            std::cout << "Progress: " << (i + 1) << "/" << num_ops << std::endl;
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "\n=== Stress Test Results ===" << std::endl;
    std::cout << "Duration: " << duration << " ms" << std::endl;
    std::cout << "Reads: " << reads << " (avg latency: " << (reads > 0 ? total_read_latency/reads : 0) << " ns)" << std::endl;
    std::cout << "Writes: " << writes << " (avg latency: " << (writes > 0 ? total_write_latency/writes : 0) << " ns)" << std::endl;
    std::cout << "Throughput: " << (num_ops * 1000.0 / duration) << " ops/sec" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string mode = "reader";
    std::string server = "127.0.0.1:9999";
    std::string shm_name;
    int iterations = 10;
    bool use_shm = false;
    bool stress = false;
    int stress_ops = 10000;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--server" && i + 1 < argc) {
            server = argv[++i];
        } else if (arg == "--shm" && i + 1 < argc) {
            shm_name = argv[++i];
            use_shm = true;
        } else if (arg == "--iterations" && i + 1 < argc) {
            iterations = std::stoi(argv[++i]);
        } else if (arg == "--stress") {
            stress = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                stress_ops = std::stoi(argv[++i]);
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  --mode <reader|writer>  Test mode (default: reader)" << std::endl;
            std::cout << "  --server <host:port>    CXLMemSim server (default: 127.0.0.1:9999)" << std::endl;
            std::cout << "  --shm <name>            Use SHM mode instead of TCP" << std::endl;
            std::cout << "  --iterations <n>        Number of iterations (default: 10)" << std::endl;
            std::cout << "  --stress [n]            Run stress test with n ops (default: 10000)" << std::endl;
            return 0;
        }
    }

    std::cout << "=== CXL Back Invalidation Test ===" << std::endl;
    std::cout << "Mode: " << mode << std::endl;

    CXLClient* client;
    if (use_shm) {
        std::cout << "Transport: SHM (" << shm_name << ")" << std::endl;
        client = new CXLClient(shm_name);
    } else {
        size_t colon = server.find(':');
        std::string host = server.substr(0, colon);
        int port = std::stoi(server.substr(colon + 1));
        std::cout << "Transport: TCP (" << host << ":" << port << ")" << std::endl;
        client = new CXLClient(host, port);
    }

    if (!client->connect()) {
        std::cerr << "Failed to connect!" << std::endl;
        return 1;
    }

    if (stress) {
        run_stress_test(*client, stress_ops);
    } else if (mode == "writer") {
        run_writer_test(*client, iterations);
    } else {
        run_reader_test(*client, iterations);
    }

    delete client;
    return 0;
}
