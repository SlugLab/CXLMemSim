/*
 * Distributed Multi-Memory Server Header
 * Provides inter-node communication for distributed CXL memory simulation
 * using shared memory message passing between nodes.
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 * Copyright 2025 Regents of the University of California
 * UC Santa Cruz Sluglab.
 */

#ifndef DISTRIBUTED_SERVER_H
#define DISTRIBUTED_SERVER_H

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <functional>
#include <chrono>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants and Configuration
 * ============================================================================ */

#define DIST_MAX_NODES 16
#define DIST_MSG_QUEUE_SIZE 4096
#define DIST_CACHELINE_SIZE 64
#define DIST_SHM_MAGIC 0x4458544D454D5348ULL  /* "DXTMEMSH" */
#define DIST_SHM_VERSION 1

/* Inter-node message types */
typedef enum {
    DIST_MSG_NONE = 0,

    /* Node management */
    DIST_MSG_NODE_REGISTER = 1,
    DIST_MSG_NODE_DEREGISTER = 2,
    DIST_MSG_NODE_HEARTBEAT = 3,
    DIST_MSG_NODE_ACK = 4,

    /* Memory operations (forwarded between nodes) */
    DIST_MSG_READ_REQ = 10,
    DIST_MSG_READ_RESP = 11,
    DIST_MSG_WRITE_REQ = 12,
    DIST_MSG_WRITE_RESP = 13,

    /* Atomic operations */
    DIST_MSG_ATOMIC_FAA_REQ = 20,
    DIST_MSG_ATOMIC_FAA_RESP = 21,
    DIST_MSG_ATOMIC_CAS_REQ = 22,
    DIST_MSG_ATOMIC_CAS_RESP = 23,
    DIST_MSG_FENCE_REQ = 24,
    DIST_MSG_FENCE_RESP = 25,

    /* Coherency protocol messages */
    DIST_MSG_INVALIDATE = 30,
    DIST_MSG_INVALIDATE_ACK = 31,
    DIST_MSG_DOWNGRADE = 32,
    DIST_MSG_DOWNGRADE_ACK = 33,
    DIST_MSG_WRITEBACK = 34,
    DIST_MSG_WRITEBACK_ACK = 35,

    /* Directory operations */
    DIST_MSG_DIR_UPDATE = 40,
    DIST_MSG_DIR_QUERY = 41,
    DIST_MSG_DIR_RESPONSE = 42,

    /* Bulk transfer */
    DIST_MSG_BULK_READ_REQ = 50,
    DIST_MSG_BULK_READ_RESP = 51,
    DIST_MSG_BULK_WRITE_REQ = 52,
    DIST_MSG_BULK_WRITE_RESP = 53,
} dist_msg_type_t;

/* Node states */
typedef enum {
    NODE_STATE_UNKNOWN = 0,
    NODE_STATE_INITIALIZING = 1,
    NODE_STATE_READY = 2,
    NODE_STATE_BUSY = 3,
    NODE_STATE_DRAINING = 4,
    NODE_STATE_OFFLINE = 5,
} node_state_t;

/* Cache coherency states for distributed directory */
typedef enum {
    DIST_CACHE_INVALID = 0,
    DIST_CACHE_SHARED = 1,
    DIST_CACHE_EXCLUSIVE = 2,
    DIST_CACHE_MODIFIED = 3,
    DIST_CACHE_OWNED = 4,      /* Extended MOESI */
    DIST_CACHE_FORWARD = 5,    /* Extended MESIF */
} dist_cache_state_t;

/* ============================================================================
 * Message Structures
 * ============================================================================ */

/* Base message header (32 bytes) */
typedef struct {
    uint32_t msg_type;          /* dist_msg_type_t */
    uint32_t msg_id;            /* Unique message ID for tracking */
    uint32_t src_node_id;       /* Source node */
    uint32_t dst_node_id;       /* Destination node (0xFFFF = broadcast) */
    uint64_t timestamp;         /* Message timestamp */
    uint32_t payload_size;      /* Size of payload following header */
    uint32_t flags;             /* Message flags */
} __attribute__((packed)) dist_msg_header_t;

/* Memory operation payload (128 bytes) */
typedef struct {
    uint64_t addr;              /* Memory address */
    uint64_t size;              /* Operation size */
    uint64_t value;             /* Value for atomic ops */
    uint64_t expected;          /* Expected value for CAS */
    uint64_t latency_ns;        /* Reported latency */
    uint32_t client_id;         /* Original client ID */
    uint32_t status;            /* Operation status */
    uint8_t cache_state;        /* Current cache state */
    uint8_t new_cache_state;    /* New cache state after operation */
    uint16_t sharers_bitmap;    /* Bitmap of sharing nodes */
    uint32_t version;           /* Cache line version */
    uint8_t data[64];           /* Cacheline data */
    uint8_t reserved[12];       /* Padding to 128 bytes */
} __attribute__((packed)) dist_mem_payload_t;

/* Node info payload (64 bytes) */
typedef struct {
    uint32_t node_id;           /* Node ID */
    uint32_t node_state;        /* node_state_t */
    uint64_t memory_base;       /* Base address of this node's memory */
    uint64_t memory_size;       /* Size of memory managed by this node */
    uint64_t num_cachelines;    /* Number of cachelines */
    uint32_t port;              /* TCP port (for fallback) */
    uint32_t flags;             /* Node flags */
    char hostname[24];          /* Node hostname */
} __attribute__((packed)) dist_node_payload_t;

/* Coherency operation payload (48 bytes) */
typedef struct {
    uint64_t cacheline_addr;    /* Aligned cacheline address */
    uint32_t requesting_node;   /* Node requesting the operation */
    uint32_t owner_node;        /* Current owner node */
    uint16_t sharers_bitmap;    /* Nodes with shared copies */
    uint8_t current_state;      /* Current MESI state */
    uint8_t requested_state;    /* Requested MESI state */
    uint32_t version;           /* Cacheline version */
    uint8_t data[24];           /* Dirty data for writeback */
} __attribute__((packed)) dist_coherency_payload_t;

/* Directory entry for tracking cacheline ownership across nodes */
typedef struct {
    uint64_t cacheline_addr;    /* Cacheline address */
    uint8_t state;              /* Global cache state */
    uint8_t home_node;          /* Home node for this address */
    uint8_t owner_node;         /* Current owner (for E/M states) */
    uint8_t flags;              /* Entry flags */
    uint16_t sharers_bitmap;    /* Bitmap of nodes with copies */
    uint16_t pending_bitmap;    /* Nodes with pending operations */
    uint32_t version;           /* Version number */
    uint64_t last_access_time;  /* Timestamp of last access */
} __attribute__((packed)) dist_directory_entry_t;

/* Complete message structure */
typedef struct {
    dist_msg_header_t header;
    union {
        dist_mem_payload_t mem;
        dist_node_payload_t node;
        dist_coherency_payload_t coherency;
        uint8_t raw[256];       /* Raw payload buffer */
    } payload;
} __attribute__((aligned(512))) dist_message_t;

/* ============================================================================
 * Shared Memory Structures for Inter-Node Communication
 * ============================================================================ */

/* Per-node message queue in shared memory */
typedef struct {
    /* Ring buffer for incoming messages */
    volatile uint32_t head;         /* Producer writes here */
    volatile uint32_t tail;         /* Consumer reads here */
    volatile uint32_t msg_count;    /* Number of messages in queue */
    uint32_t capacity;              /* Queue capacity */

    /* Statistics */
    volatile uint64_t total_sent;
    volatile uint64_t total_received;
    volatile uint64_t total_dropped;

    uint8_t padding[32];            /* Align to 64 bytes */

    /* Message array */
    dist_message_t messages[DIST_MSG_QUEUE_SIZE];
} __attribute__((aligned(64))) dist_node_queue_t;

/* Node status in shared memory */
typedef struct {
    volatile uint32_t node_id;
    volatile uint32_t state;        /* node_state_t */
    volatile uint64_t last_heartbeat;
    volatile uint64_t memory_base;
    volatile uint64_t memory_size;
    volatile uint32_t active_connections;
    volatile uint32_t flags;
    char hostname[32];
    uint8_t padding[8];             /* Align to 64 bytes */
} __attribute__((aligned(64))) dist_node_status_t;

/* Main shared memory header for distributed communication */
typedef struct {
    /* Header info */
    uint64_t magic;                 /* DIST_SHM_MAGIC */
    uint32_t version;               /* DIST_SHM_VERSION */
    uint32_t num_nodes;             /* Number of active nodes */
    volatile uint32_t coordinator_node;  /* Current coordinator node ID */
    volatile uint32_t global_epoch;      /* Global synchronization epoch */

    /* Global flags and state */
    volatile uint32_t system_ready;
    volatile uint32_t shutdown_requested;

    uint8_t header_padding[32];     /* Pad header to 64 bytes */

    /* Node status array */
    dist_node_status_t nodes[DIST_MAX_NODES];

    /* Message queues for each node pair (node_i -> node_j) */
    /* Layout: queues[src_node * DIST_MAX_NODES + dst_node] */
    dist_node_queue_t queues[DIST_MAX_NODES * DIST_MAX_NODES];
} __attribute__((aligned(4096))) dist_shm_header_t;

/* Size calculation macro */
#define DIST_SHM_SIZE sizeof(dist_shm_header_t)

#ifdef __cplusplus
}
#endif

/* ============================================================================
 * C++ Classes for Distributed Server
 * ============================================================================ */

#ifdef __cplusplus

#include <functional>
#include <memory>
#include <unordered_map>

/* RDMA transport support */
#include "rdma_communication.h"

/* Forward declarations */
class CXLController;
class SharedMemoryManager;
class CoherencyEngine;
class HDMDecoder;
struct MHSLDDevice;
enum class MHSLDCacheState : uint8_t;

/* Transport mode for distributed server */
enum class DistTransportMode {
    SHM,        // Shared memory ring buffers (existing)
    RDMA,       // RDMA verbs
    HYBRID      // SHM for local, RDMA for remote
};

/* RDMA calibration result for LogP parameter extraction */
struct RDMACalibrationResult {
    double L;           // Measured latency (ns)
    double o_s;         // Measured send overhead (ns)
    double o_r;         // Measured receive overhead (ns)
    double g;           // Measured gap - 1/bandwidth (ns)
    uint64_t samples;   // Number of samples taken
    bool valid;         // Whether calibration succeeded

    RDMACalibrationResult() : L(0), o_s(0), o_r(0), g(0), samples(0), valid(false) {}
};

/* Per-node RDMA connection state */
struct RDMANodeConnection {
    std::unique_ptr<RDMAClient> client;     // Outgoing RDMA connection
    uint64_t remote_addr;                    // Remote base address for one-sided ops
    uint32_t remote_rkey;                    // Remote region key
    uint32_t remote_mr_lkey;                 // Remote MR local key
    size_t remote_buffer_size;               // Remote buffer size
    bool connected;                          // Connection state
    RDMACalibrationResult calibration;       // Per-node calibration data

    RDMANodeConnection() : remote_addr(0), remote_rkey(0), remote_mr_lkey(0),
                           remote_buffer_size(0), connected(false) {}
};

/* Message handler callback type */
using DistMessageHandler = std::function<void(const dist_message_t&, dist_message_t&)>;

/* Directory entry with C++ features */
struct DistDirectoryEntry {
    uint64_t cacheline_addr;
    dist_cache_state_t state;
    uint32_t home_node;
    uint32_t owner_node;
    std::set<uint32_t> sharers;
    uint32_t version;
    uint64_t last_access_time;
    std::mutex lock;

    DistDirectoryEntry() : cacheline_addr(0), state(DIST_CACHE_INVALID),
                           home_node(0), owner_node(UINT32_MAX), version(0),
                           last_access_time(0) {}

    // Non-copyable due to mutex
    DistDirectoryEntry(const DistDirectoryEntry&) = delete;
    DistDirectoryEntry& operator=(const DistDirectoryEntry&) = delete;

    // Move constructor (mutex can't be moved, so we reset it)
    DistDirectoryEntry(DistDirectoryEntry&& other) noexcept
        : cacheline_addr(other.cacheline_addr), state(other.state),
          home_node(other.home_node), owner_node(other.owner_node),
          sharers(std::move(other.sharers)), version(other.version),
          last_access_time(other.last_access_time) {}
};

/* Node information with connection state */
struct DistNodeInfo {
    uint32_t node_id;
    std::string hostname;
    node_state_t state;
    uint64_t memory_base;
    uint64_t memory_size;
    uint64_t last_heartbeat;
    uint32_t pending_requests;
    uint64_t total_requests;
    uint64_t total_latency_ns;

    DistNodeInfo() : node_id(UINT32_MAX), state(NODE_STATE_UNKNOWN),
                     memory_base(0), memory_size(0), last_heartbeat(0),
                     pending_requests(0), total_requests(0), total_latency_ns(0) {}

    // Copy constructor
    DistNodeInfo(const DistNodeInfo& other)
        : node_id(other.node_id), hostname(other.hostname), state(other.state),
          memory_base(other.memory_base), memory_size(other.memory_size),
          last_heartbeat(other.last_heartbeat), pending_requests(other.pending_requests),
          total_requests(other.total_requests), total_latency_ns(other.total_latency_ns) {}

    // Copy assignment
    DistNodeInfo& operator=(const DistNodeInfo& other) {
        if (this != &other) {
            node_id = other.node_id;
            hostname = other.hostname;
            state = other.state;
            memory_base = other.memory_base;
            memory_size = other.memory_size;
            last_heartbeat = other.last_heartbeat;
            pending_requests = other.pending_requests;
            total_requests = other.total_requests;
            total_latency_ns = other.total_latency_ns;
        }
        return *this;
    }
};

/* Inter-node message passing manager */
class DistributedMessageManager {
private:
    /* Shared memory for inter-node communication */
    std::string shm_name_;
    int shm_fd_;
    dist_shm_header_t* shm_header_;

    /* Local node info */
    uint32_t local_node_id_;
    bool is_coordinator_;

    /* Message handlers */
    std::map<dist_msg_type_t, DistMessageHandler> handlers_;
    std::shared_mutex handlers_mutex_;

    /* Message ID generation */
    std::atomic<uint32_t> next_msg_id_;

    /* Pending responses tracking */
    struct PendingRequest {
        uint32_t msg_id;
        dist_message_t* response;
        std::condition_variable cv;
        std::mutex mutex;
        bool completed;
    };
    std::map<uint32_t, std::shared_ptr<PendingRequest>> pending_requests_;
    std::mutex pending_mutex_;

    /* Worker threads */
    std::vector<std::thread> workers_;
    std::atomic<bool> running_;

public:
    DistributedMessageManager(const std::string& shm_name, uint32_t node_id);
    ~DistributedMessageManager();

    /* Initialization */
    bool initialize(bool create_new = false);
    void cleanup();

    /* Node management */
    bool register_node(const DistNodeInfo& info);
    bool deregister_node(uint32_t node_id);
    bool is_node_active(uint32_t node_id) const;
    std::vector<uint32_t> get_active_nodes() const;

    /* Message sending */
    bool send_message(uint32_t dst_node, const dist_message_t& msg);
    bool send_message_wait_response(uint32_t dst_node, const dist_message_t& req,
                                     dist_message_t& resp, int timeout_ms = 5000);
    bool broadcast_message(const dist_message_t& msg);

    /* Message handling */
    void register_handler(dist_msg_type_t type, DistMessageHandler handler);
    void unregister_handler(dist_msg_type_t type);

    /* Processing */
    void start_processing();
    void stop_processing();
    int poll_messages(int max_messages = 100);

    /* Utilities */
    uint32_t get_local_node_id() const { return local_node_id_; }
    bool is_coordinator() const { return is_coordinator_; }
    void set_coordinator(bool is_coord) { is_coordinator_ = is_coord; }
    uint32_t generate_msg_id() { return next_msg_id_++; }

    /* Heartbeat */
    void send_heartbeat();

    /* Statistics */
    struct Stats {
        uint64_t messages_sent;
        uint64_t messages_received;
        uint64_t messages_dropped;
        uint64_t avg_latency_ns;
    };
    Stats get_stats() const;

private:
    bool enqueue_message(uint32_t dst_node, const dist_message_t& msg);
    bool dequeue_message(uint32_t src_node, dist_message_t& msg);
    void process_message(const dist_message_t& msg);
    void worker_thread();
};

/* DistributedDirectory removed - replaced by CoherencyEngine */

/* RDMA Transport Layer for Distributed Server */
class DistributedRDMATransport {
private:
    uint32_t local_node_id_;
    std::string bind_addr_;
    uint16_t port_;

    // Per-node RDMA connections
    std::map<uint32_t, RDMANodeConnection> connections_;
    std::mutex connections_mutex_;

    // RDMA server for incoming connections
    std::unique_ptr<RDMAServer> server_;
    std::thread accept_thread_;
    std::atomic<bool> running_;

    // Calibration results per node
    std::map<uint32_t, RDMACalibrationResult> calibration_results_;
    std::mutex calibration_mutex_;

public:
    DistributedRDMATransport(uint32_t node_id, const std::string& bind_addr, uint16_t port);
    ~DistributedRDMATransport();

    // Lifecycle
    bool initialize();
    void shutdown();

    // Connection management
    bool connect_to_node(uint32_t node_id, const std::string& addr, uint16_t port);
    void disconnect_node(uint32_t node_id);
    bool is_connected(uint32_t node_id) const;
    std::vector<uint32_t> get_connected_nodes() const;

    // Two-sided messaging (for coherency protocol)
    bool send_message(uint32_t dst_node, const dist_message_t& msg);
    bool send_message_wait_response(uint32_t dst_node, const dist_message_t& req,
                                     dist_message_t& resp, int timeout_ms = 5000);

    // One-sided operations (for data access - bypasses remote CPU)
    bool rdma_read(uint32_t dst_node, uint64_t remote_offset, void* local_buf, size_t size);
    bool rdma_write(uint32_t dst_node, uint64_t remote_offset, const void* local_buf, size_t size);

    // LogP calibration via RDMA ping-pong
    RDMACalibrationResult calibrate_node(uint32_t dst_node, uint32_t num_samples = 1000);
    RDMACalibrationResult get_calibration(uint32_t node_id) const;
    RDMACalibrationResult get_aggregate_calibration() const;

    // Exchange memory region info for one-sided ops
    bool exchange_mr_info(uint32_t node_id);

    // Getters
    uint32_t get_local_node_id() const { return local_node_id_; }
    uint16_t get_port() const { return port_; }

private:
    void accept_loop();
    bool send_rdma_msg(uint32_t dst_node, const void* data, size_t size);
    bool recv_rdma_msg(uint32_t src_node, void* data, size_t size, int timeout_ms);
};

/* DistributedMHSLDManager removed - replaced by CoherencyEngine */

/* Main distributed memory server class */
class DistributedMemoryServer {
private:
    /* Configuration */
    uint32_t node_id_;
    std::string shm_name_;
    int tcp_port_;
    size_t memory_capacity_mb_;
    DistTransportMode transport_mode_;

    /* RDMA configuration */
    std::string rdma_addr_;
    uint16_t rdma_port_;

    /* Core components */
    CXLController* controller_;
    std::unique_ptr<SharedMemoryManager> local_memory_;
    std::unique_ptr<DistributedMessageManager> msg_manager_;
    std::unique_ptr<DistributedRDMATransport> rdma_transport_;

    /* Node registry */
    std::map<uint32_t, DistNodeInfo> nodes_;
    mutable std::shared_mutex nodes_mutex_;

    /* Runtime state */
    std::atomic<bool> running_;
    std::atomic<node_state_t> state_;

    /* Worker threads */
    std::thread heartbeat_thread_;
    std::thread request_processor_thread_;
    std::vector<std::thread> client_threads_;

    /* Statistics */
    std::atomic<uint64_t> local_reads_;
    std::atomic<uint64_t> local_writes_;
    std::atomic<uint64_t> remote_reads_;
    std::atomic<uint64_t> remote_writes_;
    std::atomic<uint64_t> forwarded_requests_;
    std::atomic<uint64_t> coherency_messages_;

public:
    DistributedMemoryServer(uint32_t node_id, const std::string& shm_name,
                             int tcp_port, size_t capacity_mb,
                             CXLController* controller,
                             DistTransportMode transport_mode = DistTransportMode::SHM,
                             const std::string& rdma_addr = "0.0.0.0",
                             uint16_t rdma_port = 5555);
    ~DistributedMemoryServer();

    /* Lifecycle */
    bool initialize();
    bool start();
    void stop();
    bool join_cluster(const std::string& coordinator_shm);
    bool leave_cluster();

    /* Memory operations (may be forwarded to remote nodes) */
    int read(uint64_t addr, void* data, size_t size, uint64_t* latency_ns);
    int write(uint64_t addr, const void* data, size_t size, uint64_t* latency_ns);
    int atomic_faa(uint64_t addr, uint64_t value, uint64_t* old_value);
    int atomic_cas(uint64_t addr, uint64_t expected, uint64_t desired,
                   uint64_t* old_value);
    void fence();

    /* Node management */
    bool add_remote_node(const DistNodeInfo& info);
    bool remove_remote_node(uint32_t node_id);
    std::vector<DistNodeInfo> get_cluster_nodes() const;

    /* Address mapping */
    uint32_t get_node_for_address(uint64_t addr) const;
    bool is_local_address(uint64_t addr) const;

    /* Getters */
    uint32_t get_node_id() const { return node_id_; }
    node_state_t get_state() const { return state_.load(); }
    bool is_running() const { return running_.load(); }
    DistTransportMode get_transport_mode() const { return transport_mode_; }

    /* RDMA transport management */
    bool connect_rdma_node(uint32_t node_id, const std::string& addr, uint16_t port);
    bool calibrate_rdma_logp(uint32_t target_node = UINT32_MAX);
    DistributedRDMATransport* get_rdma_transport() { return rdma_transport_.get(); }

    /* CoherencyEngine access (delegates to controller) */
    CoherencyEngine* coherency();

    /* Statistics */
    struct Stats {
        uint64_t local_reads;
        uint64_t local_writes;
        uint64_t remote_reads;
        uint64_t remote_writes;
        uint64_t forwarded_requests;
        uint64_t coherency_messages;
        uint64_t active_connections;
    };
    Stats get_stats() const;

private:
    /* Message handlers */
    void setup_message_handlers();
    void handle_read_request(const dist_message_t& req, dist_message_t& resp);
    void handle_write_request(const dist_message_t& req, dist_message_t& resp);
    void handle_atomic_request(const dist_message_t& req, dist_message_t& resp);
    void handle_coherency_request(const dist_message_t& req, dist_message_t& resp);
    void handle_node_message(const dist_message_t& req, dist_message_t& resp);

    /* Background tasks */
    void heartbeat_loop();
    void process_requests_loop();

    /* Forwarding */
    int forward_read(uint32_t target_node, uint64_t addr, void* data,
                     size_t size, uint64_t* latency_ns);
    int forward_write(uint32_t target_node, uint64_t addr, const void* data,
                      size_t size, uint64_t* latency_ns);

    /* RDMA-based forwarding (used when transport_mode_ is RDMA or HYBRID) */
    int forward_read_rdma(uint32_t target_node, uint64_t addr, void* data,
                          size_t size, uint64_t* latency_ns);
    int forward_write_rdma(uint32_t target_node, uint64_t addr, const void* data,
                           size_t size, uint64_t* latency_ns);

    /* Coherency */
    bool ensure_coherency_for_read(uint64_t addr, uint32_t requesting_node);
    bool ensure_coherency_for_write(uint64_t addr, uint32_t requesting_node);

    /* RDMA initialization helpers */
    bool initialize_rdma_transport();
    void calibrate_all_rdma_nodes();
};

#endif /* __cplusplus */

#endif /* DISTRIBUTED_SERVER_H */
