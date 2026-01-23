/*
 * CXLMemSim endpoint
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#ifndef CXLMEMSIM_CXLENDPOINT_H
#define CXLMEMSIM_CXLENDPOINT_H

#include "cxlcounter.h"
#include "helper.h"
#include <list>
#include <queue>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <cmath>
#include <deque>
#define ROB_SIZE 512

struct occupation_info {
    uint64_t timestamp{};
    uint64_t address{};
    uint64_t access_count{};
};
struct rob_info {
    std::map<int, int64_t> m_bandwidth, m_count;
    int64_t llcm_base, llcm_count, ins_count;
};
struct thread_info {
    rob_info rob;
    std::queue<int> llcm_type;
    std::queue<int> llcm_type_rob;
};
// Forward declarations
class CXLController;
class CXLEndPoint {
public:
    virtual ~CXLEndPoint() = default;

private:
    virtual void set_epoch(int epoch) = 0;
    virtual void free_stats(double size) = 0;
    virtual void delete_entry(uint64_t addr, uint64_t length) = 0;
    virtual double calculate_latency(const std::vector<std::tuple<uint64_t, uint64_t>> &elem,
                                     double dramlatency) = 0; // traverse the tree to calculate the latency
    virtual double calculate_bandwidth(const std::vector<std::tuple<uint64_t, uint64_t>> &elem) = 0;
    virtual int insert(uint64_t timestamp, uint64_t tid, uint64_t phys_addr, uint64_t virt_addr,
                       int index) = 0; // 0 not this endpoint, 1 store, 2 load, 3 prefetch
    virtual std::vector<std::tuple<uint64_t, uint64_t>> get_access(uint64_t timestamp) = 0;
};

// CXL Protocol constants
constexpr size_t MAX_QUEUE_SIZE = 64;
constexpr size_t FLIT_SIZE = 66;  // 528/8 = 66 bytes per flit
constexpr size_t DATA_FLIT = 65;  // Data flit overhead in bytes
constexpr size_t INITIAL_CREDITS = 2;  // ResCrd[2] for response credits

// Request structure for queue management
struct CXLRequest {
    uint64_t timestamp;
    uint64_t address;
    uint64_t tid;
    bool is_read;
    bool is_write;
    uint64_t issue_time;
    uint64_t complete_time;
};

/* ============================================================================
 * LogP Queuing Model
 *
 * Models point-to-point communication between CXL nodes:
 *   L: Network latency (propagation delay, ns)
 *   o: Overhead (CPU processing time per message, ns)
 *   g: Gap (minimum inter-message time = 1/bandwidth, ns)
 *   P: Number of processors/nodes in the system
 *
 * Total point-to-point time: T = o_s + L + o_r (no contention)
 * With queuing: Uses M/D/1 model for queue wait time estimation
 * ============================================================================ */

struct LogPConfig {
    double L;     // Network latency (ns) - propagation delay between nodes
    double o_s;   // Sender overhead (ns) - CPU time to initiate send
    double o_r;   // Receiver overhead (ns) - CPU time to process receive
    double g;     // Gap (ns) - minimum time between consecutive messages (1/BW)
    uint32_t P;   // Number of processors/nodes

    LogPConfig() : L(150.0), o_s(20.0), o_r(20.0), g(4.0), P(2) {}

    LogPConfig(double latency, double send_overhead, double recv_overhead,
               double gap, uint32_t num_nodes)
        : L(latency), o_s(send_overhead), o_r(recv_overhead),
          g(gap), P(num_nodes) {}
};

// Queue state for LogP model
struct LogPQueueState {
    double arrival_rate;      // lambda: average arrival rate (messages/ns)
    double service_rate;      // mu: service rate (1/g)
    double queue_occupancy;   // rho: utilization = lambda/mu
    uint64_t total_messages;
    uint64_t total_wait_ns;
    uint64_t last_send_time;  // Last time a message was sent (for gap enforcement)
    uint64_t last_recv_time;  // Last time a message was received

    LogPQueueState() : arrival_rate(0.0), service_rate(0.0), queue_occupancy(0.0),
                       total_messages(0), total_wait_ns(0),
                       last_send_time(0), last_recv_time(0) {}
};

class LogPModel {
public:
    LogPConfig config;
    LogPQueueState state;

    // Per-node queue states for multi-node communication
    std::unordered_map<uint32_t, LogPQueueState> node_queues;
    mutable std::mutex queue_mutex;

    LogPModel() = default;
    explicit LogPModel(const LogPConfig& cfg) : config(cfg) {
        state.service_rate = 1.0 / cfg.g;
    }

    // Reconfigure with new parameters (mutex stays in place)
    void reconfigure(const LogPConfig& cfg) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        config = cfg;
        state.service_rate = 1.0 / cfg.g;
        node_queues.clear();
    }

    // Point-to-point latency (no queuing contention)
    double p2p_latency() const {
        return config.o_s + config.L + config.o_r;
    }

    // Calculate send delay with gap enforcement
    double send_delay(uint64_t current_time, uint32_t dst_node) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        auto& q = node_queues[dst_node];

        double gap_wait = 0.0;
        if (q.last_send_time > 0 && current_time < q.last_send_time + config.g) {
            gap_wait = (q.last_send_time + config.g) - current_time;
        }
        q.last_send_time = current_time + gap_wait + config.o_s;
        return gap_wait + config.o_s;
    }

    // M/D/1 queue wait time estimation
    // W_q = rho / (2 * mu * (1 - rho)) for M/D/1
    double queue_wait_time(uint32_t dst_node) const {
        std::lock_guard<std::mutex> lock(queue_mutex);
        auto it = node_queues.find(dst_node);
        if (it == node_queues.end()) return 0.0;

        const auto& q = it->second;
        double rho = q.queue_occupancy;
        if (rho >= 1.0) rho = 0.99; // Prevent infinite wait
        if (rho < 0.01) return 0.0;

        // M/D/1 waiting time formula
        double w_q = rho / (2.0 * state.service_rate * (1.0 - rho));
        return w_q;
    }

    // Full message latency including queuing
    double message_latency(uint64_t current_time, uint32_t dst_node) {
        double s_delay = send_delay(current_time, dst_node);
        double q_wait = queue_wait_time(dst_node);
        return s_delay + config.L + q_wait + config.o_r;
    }

    // Update arrival rate based on observed traffic
    void update_arrival_rate(uint32_t dst_node, uint64_t window_ns, uint64_t message_count) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        auto& q = node_queues[dst_node];
        q.arrival_rate = static_cast<double>(message_count) / window_ns;
        q.queue_occupancy = q.arrival_rate / state.service_rate;
        q.total_messages += message_count;
    }

    // Record a completed message for stats
    void record_message(uint32_t dst_node, uint64_t latency_ns) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        auto& q = node_queues[dst_node];
        q.total_messages++;
        q.total_wait_ns += latency_ns;
    }

    // Broadcast latency (LogP tree broadcast)
    // T_bcast = ceil(log2(P)) * (o_s + L + o_r) for tree broadcast
    double broadcast_latency() const {
        if (config.P <= 1) return 0.0;
        double tree_depth = std::ceil(std::log2(config.P));
        return tree_depth * p2p_latency();
    }

    // Barrier synchronization latency
    // T_barrier = 2 * ceil(log2(P)) * (o_s + L + o_r)
    double barrier_latency() const {
        return 2.0 * broadcast_latency();
    }

    // Get average message latency for a node
    double avg_latency(uint32_t dst_node) const {
        std::lock_guard<std::mutex> lock(queue_mutex);
        auto it = node_queues.find(dst_node);
        if (it == node_queues.end() || it->second.total_messages == 0) {
            return p2p_latency();
        }
        return static_cast<double>(it->second.total_wait_ns) / it->second.total_messages;
    }
};

/* ============================================================================
 * MH-SLD: Multi-Headed Single Logical Device
 *
 * Models a CXL memory device shared by multiple hosts (heads).
 * Each host has its own port to the device, and cacheline states
 * track sharing/ownership across all heads.
 *
 * Pooling: Multiple hosts dynamically share device capacity
 * Sharing: Cacheline-level coherency between heads using MESI+
 * ============================================================================ */

// MH-SLD cacheline coherency states (per-head view)
enum class MHSLDCacheState : uint8_t {
    INVALID = 0,    // Head has no valid copy
    SHARED = 1,     // Head has read-only shared copy
    EXCLUSIVE = 2,  // Head has exclusive (clean) copy
    MODIFIED = 3,   // Head has exclusive (dirty) copy
    OWNED = 4,      // Head has dirty copy, other heads may have shared
};

// Per-cacheline metadata in MH-SLD
struct MHSLDCachelineInfo {
    uint64_t address;                    // Cacheline-aligned address
    std::set<uint32_t> sharers;          // Set of head IDs with SHARED copies
    uint32_t owner_head;                 // Head ID of EXCLUSIVE/MODIFIED owner
    MHSLDCacheState owner_state;         // State at the owner
    uint32_t version;                    // Version counter for ABA prevention
    uint64_t last_access_time;           // Timestamp of last access
    uint64_t access_count;               // Total access count
    bool has_dirty_data;                 // Whether dirty data needs writeback
    std::mutex lock;                     // Per-cacheline lock

    MHSLDCachelineInfo()
        : address(0), owner_head(UINT32_MAX), owner_state(MHSLDCacheState::INVALID),
          version(0), last_access_time(0), access_count(0), has_dirty_data(false) {}

    // Non-copyable
    MHSLDCachelineInfo(const MHSLDCachelineInfo&) = delete;
    MHSLDCachelineInfo& operator=(const MHSLDCachelineInfo&) = delete;
};

// Per-head (host port) state in MH-SLD
struct MHSLDHeadState {
    uint32_t head_id;                    // Head (host) ID
    uint64_t allocated_capacity;         // Bytes allocated to this head
    uint64_t used_capacity;              // Bytes currently used
    double bandwidth_share;              // Fair bandwidth share (0.0-1.0)
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t coherency_stalls;           // Stalls due to coherency protocol
    uint64_t back_invalidations;         // Back-invalidations received
    bool active;                         // Whether this head is currently connected

    MHSLDHeadState() : head_id(0), allocated_capacity(0), used_capacity(0),
                       bandwidth_share(1.0), total_reads(0), total_writes(0),
                       coherency_stalls(0), back_invalidations(0), active(false) {}
};

// MH-SLD Device Manager
class MHSLDDevice {
public:
    static constexpr uint32_t MAX_HEADS = 16;
    static constexpr size_t CACHELINE_SIZE = 64;

    // Device configuration
    uint64_t total_capacity;             // Total device capacity in bytes
    uint32_t num_heads;                  // Number of configured heads
    double base_read_latency;            // Base read latency (ns)
    double base_write_latency;           // Base write latency (ns)
    double max_bandwidth;                // Maximum device bandwidth (GB/s)

    // Per-head states
    std::vector<MHSLDHeadState> heads;

    // Cacheline directory (address -> metadata)
    std::unordered_map<uint64_t, std::unique_ptr<MHSLDCachelineInfo>> directory;
    mutable std::shared_mutex directory_mutex;

    // LogP model for inter-head communication
    LogPModel logp_model;

    // Statistics
    std::atomic<uint64_t> total_coherency_messages{0};
    std::atomic<uint64_t> total_invalidations{0};
    std::atomic<uint64_t> total_downgrades{0};
    std::atomic<uint64_t> total_writebacks{0};

    MHSLDDevice(uint64_t capacity, uint32_t num_heads, double read_lat,
                double write_lat, double bandwidth, const LogPConfig& logp_cfg);
    ~MHSLDDevice() = default;

    // Head management
    bool activate_head(uint32_t head_id, uint64_t capacity_alloc);
    void deactivate_head(uint32_t head_id);
    void rebalance_bandwidth();

    // Memory pooling operations
    uint64_t allocate_pool(uint32_t head_id, uint64_t size);
    void release_pool(uint32_t head_id, uint64_t addr, uint64_t size);
    double get_pool_utilization() const;

    // Cacheline-state-aware access with LogP latency
    double read_with_coherency(uint32_t head_id, uint64_t addr, uint64_t timestamp);
    double write_with_coherency(uint32_t head_id, uint64_t addr, uint64_t timestamp);
    double atomic_with_coherency(uint32_t head_id, uint64_t addr, uint64_t timestamp);

    // Coherency protocol operations
    double invalidate_sharers(uint64_t addr, uint32_t except_head, uint64_t timestamp);
    double downgrade_owner(uint64_t addr, uint32_t requesting_head, uint64_t timestamp);
    double writeback(uint64_t addr, uint32_t head_id, uint64_t timestamp);

    // Bandwidth-aware latency calculation
    double calculate_contention_latency(uint32_t head_id, uint64_t timestamp) const;
    double calculate_fair_share_bandwidth(uint32_t head_id) const;

    // Get or create directory entry
    MHSLDCachelineInfo* get_or_create_entry(uint64_t addr);
    MHSLDCachelineInfo* get_entry(uint64_t addr);

    // Statistics
    struct Stats {
        uint64_t coherency_messages;
        uint64_t invalidations;
        uint64_t downgrades;
        uint64_t writebacks;
        double avg_read_latency;
        double avg_write_latency;
        double pool_utilization;
    };
    Stats get_stats() const;
};

class CXLMemExpander : public CXLEndPoint {
public:
    EmuCXLBandwidth bandwidth{};
    EmuCXLLatency latency{};
    uint64_t capacity;

    std::vector<occupation_info> occupation; // timestamp, pa
    std::unordered_set<uint64_t> address_cache{};
    bool cache_valid = false;
    CXLMemExpanderEvent counter{};
    CXLMemExpanderEvent last_counter{};
    mutable std::shared_mutex occupationMutex_; // 使用共享互斥锁允许多个读取者
    
    // Queue management for CXL requests
    std::deque<CXLRequest> request_queue_;
    mutable std::mutex queue_mutex_;
    
    // Credit-based flow control
    std::atomic<size_t> read_credits_{INITIAL_CREDITS};
    std::atomic<size_t> write_credits_{INITIAL_CREDITS};
    
    // Pipeline state tracking
    std::map<uint64_t, CXLRequest> in_flight_requests_;
    
    // Latency components
    double frontend_latency_ = 10.0;  // Frontend processing latency
    double forward_latency_ = 15.0;   // Forward path latency
    double response_latency_ = 20.0;  // Response path latency
    
    // LRUCache lru_cache;
    // tlb map and paging map -> invalidate
    int last_read = 0;
    int last_write = 0;
    double last_latency = 0.;
    int epoch = 0;
    uint64_t last_timestamp = 0;
    int id = -1;
    struct AddressRange {
        uint64_t start;
        uint64_t end;

        // 添加比较运算符，用于二分查找
        bool operator<(const AddressRange& other) const {
            return end < other.start;
        }

        bool operator<(uint64_t addr) const {
            return end < addr;
        }
    };
    std::vector<AddressRange> address_ranges;

    CXLMemExpander(int read_bw, int write_bw, int read_lat, int write_lat, int id, int capacity);
    std::vector<std::tuple<uint64_t, uint64_t>> get_access(uint64_t timestamp) override;
    void set_epoch(int epoch) override;
    void free_stats(double size) override;
    int insert(uint64_t timestamp, uint64_t tid, uint64_t phys_addr, uint64_t virt_addr, int index) override;
    double calculate_latency(const std::vector<std::tuple<uint64_t, uint64_t>> &elem,
                             double dramlatency) override; // traverse the tree to calculate the latency
    double calculate_bandwidth(const std::vector<std::tuple<uint64_t, uint64_t>> &elem) override;
    void delete_entry(uint64_t addr, uint64_t length) override;
    void update_address_cache() {
        if (cache_valid) return;
        address_cache.clear();
        for (const auto& occ : occupation)
            address_cache.insert(occ.address);
        cache_valid = true;
    }
    // 当 occupation 更新时调用此函数
    void invalidate_cache() {
        cache_valid = false;
    }

    void update_range_cache() {
        if (cache_valid) return;
        address_ranges.clear();
        // 排序occupation以便合并连续地址
        std::sort(occupation.begin(), occupation.end(),
                 [](const auto& a, const auto& b) { return a.address < b.address; });
        if (occupation.empty()) {
            cache_valid = true;
            return;
        }
        AddressRange current{occupation[0].address, occupation[0].address};
        for (size_t i = 1; i < occupation.size(); i++) {
            if (occupation[i].address == current.end + 1) {
                // 连续地址，扩展范围
                current.end = occupation[i].address;
            } else {
                // 新范围
                address_ranges.push_back(current);
                current = {occupation[i].address, occupation[i].address};
            }
        }
        address_ranges.push_back(current);
        cache_valid = true;
    }

    bool is_address_local(uint64_t addr) {
        if (!cache_valid)
            update_range_cache();
        // 使用标准库的二分查找，查找第一个"不小于"addr的元素
        auto it = std::lower_bound(address_ranges.begin(), address_ranges.end(), addr);
        // 如果找到了范围，且addr在这个范围内
        if (it != address_ranges.end() && addr >= it->start && addr <= it->end)
            return true;
        return false;
    }
    
    // New methods for enhanced CXL simulation
    bool can_accept_request() const;
    bool has_credits(bool is_read) const;
    void consume_credit(bool is_read);
    void release_credit(bool is_read);
    double calculate_pipeline_latency(const CXLRequest& req);
    void process_queued_requests(uint64_t current_time);
    double calculate_congestion_delay(uint64_t timestamp);
    double calculate_protocol_overhead(size_t data_size);
};
class CXLSwitch : public CXLEndPoint {
public:
    std::vector<CXLMemExpander *> expanders{};
    std::vector<CXLSwitch *> switches{};
    CXLSwitchEvent counter{};
    int id = -1;
    int epoch = 0;
    uint64_t last_timestamp = 0;
    // TODO get the approximate congestion and target done time
    std::unordered_map<uint64_t, uint64_t> timeseries_map;

    double congestion_latency = 0.02; // 200ns is the latency of the switch
    explicit CXLSwitch(int id);
    std::vector<std::tuple<uint64_t, uint64_t>> get_access(uint64_t timestamp) override;
    double calculate_latency(const std::vector<std::tuple<uint64_t, uint64_t>> &elem,
                             double dramlatency) override; // traverse the tree to calculate the latency
    double calculate_bandwidth(const std::vector<std::tuple<uint64_t, uint64_t>> &elem) override;
    double get_endpoint_rob_latency(CXLMemExpander* endpoint,
                                  const std::vector<std::tuple<uint64_t, uint64_t>>& accesses,
                                  const thread_info& t_info,
                                  double dramlatency);
    int insert(uint64_t timestamp, uint64_t tid, uint64_t phys_addr, uint64_t virt_addr, int index) override;
    void delete_entry(uint64_t addr, uint64_t length) override;
    virtual std::tuple<double, std::vector<uint64_t>> calculate_congestion();
    void set_epoch(int epoch) override;
    void free_stats(double size) override;
};

#endif // CXLMEMSIM_CXLENDPOINT_H
