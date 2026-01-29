/*
 * CXLMemSim Unified MOESI Coherency Engine
 *
 * Replaces both MHSLDDevice (local multi-headed coherency) and
 * DistributedMHSLDManager (cross-node coherency). Provides a single
 * coherency protocol for both local and remote memory accesses.
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 * Copyright 2025 Regents of the University of California
 * UC Santa Cruz Sluglab.
 */

#ifndef CXLMEMSIM_COHERENCY_ENGINE_H
#define CXLMEMSIM_COHERENCY_ENGINE_H

#include <cstdint>
#include <set>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>

// Forward declarations
class HDMDecoder;
class LogPModel;
class FabricLink;
class DistributedTCPTransport;
class DistributedMessageManager;

// Reuse MHSLDCacheState from cxlendpoint.h
enum class MHSLDCacheState : uint8_t;

// Per-head (host port) state
struct MHSLDHeadState;

struct DirectoryEntry {
    uint64_t cacheline_addr;
    MHSLDCacheState state;
    uint32_t owner_node;
    uint32_t owner_head;
    std::set<uint32_t> sharer_nodes;
    uint32_t version;
    uint64_t last_access_time;
    bool has_dirty_data;
    std::mutex lock;

    DirectoryEntry();
    // Non-copyable
    DirectoryEntry(const DirectoryEntry&) = delete;
    DirectoryEntry& operator=(const DirectoryEntry&) = delete;
};

struct CoherencyRequest {
    uint64_t addr;
    uint32_t requesting_node;
    uint32_t requesting_head;
    bool is_write;
    uint64_t timestamp;
};

struct CoherencyResponse {
    double latency_ns;           // Total coherency overhead
    MHSLDCacheState new_state;
    bool success;
    uint32_t data_source_node;   // Which node provided data
};

class CoherencyEngine {
public:
    static constexpr uint32_t MAX_HEADS = 16;
    static constexpr size_t CACHELINE_SIZE = 64;

    CoherencyEngine(uint32_t local_node, HDMDecoder* decoder, LogPModel* logp,
                    uint32_t max_heads = 16, double bandwidth_gbps = 25.0);
    ~CoherencyEngine() = default;

    // Main interface (called for every memory access)
    CoherencyResponse process_read(const CoherencyRequest& req);
    CoherencyResponse process_write(const CoherencyRequest& req);
    CoherencyResponse process_atomic(const CoherencyRequest& req);

    // Remote coherency message handlers (from TCP/SHM messages)
    void handle_remote_invalidate(uint64_t addr, uint32_t from_node);
    void handle_remote_downgrade(uint64_t addr, uint32_t from_node);
    void handle_remote_writeback(uint64_t addr, uint32_t from_node, const uint8_t* data);

    // Head management
    void activate_head(uint32_t head_id, uint64_t capacity);
    void deactivate_head(uint32_t head_id);

    // Fabric link registration
    void register_fabric_link(uint32_t node_id, FabricLink* link);

    // Transport registration (for sending remote coherency messages)
    void set_tcp_transport(DistributedTCPTransport* tcp);
    void set_msg_manager(DistributedMessageManager* msg);

    // Statistics
    struct Stats {
        uint64_t coherency_messages;
        uint64_t invalidations;
        uint64_t downgrades;
        uint64_t writebacks;
        uint64_t remote_ops;
        double avg_coherency_latency;
    };
    Stats get_stats() const;

    uint32_t get_local_node_id() const { return local_node_id_; }

private:
    uint32_t local_node_id_;
    HDMDecoder* hdm_decoder_;
    LogPModel* logp_model_;
    DistributedTCPTransport* tcp_transport_ = nullptr;
    DistributedMessageManager* msg_manager_ = nullptr;
    double bandwidth_gbps_;

    std::unordered_map<uint64_t, std::unique_ptr<DirectoryEntry>> directory_;
    mutable std::shared_mutex directory_mutex_;
    std::unordered_map<uint32_t, FabricLink*> fabric_links_;
    std::vector<MHSLDHeadState> heads_;

    // Statistics
    std::atomic<uint64_t> total_coherency_messages_{0};
    std::atomic<uint64_t> total_invalidations_{0};
    std::atomic<uint64_t> total_downgrades_{0};
    std::atomic<uint64_t> total_writebacks_{0};
    std::atomic<uint64_t> total_remote_ops_{0};
    std::atomic<uint64_t> total_latency_ns_{0};
    std::atomic<uint64_t> total_ops_{0};

    // MOESI state transitions
    double transition_to_shared(DirectoryEntry* e, uint32_t node, uint32_t head, uint64_t ts);
    double transition_to_exclusive(DirectoryEntry* e, uint32_t node, uint32_t head, uint64_t ts);
    double transition_to_modified(DirectoryEntry* e, uint32_t node, uint32_t head, uint64_t ts);

    // Coherency actions
    double invalidate_sharers(DirectoryEntry* e, uint32_t except_node, uint64_t ts);
    double downgrade_owner(DirectoryEntry* e, uint32_t requesting_node, uint64_t ts);
    double fetch_from_owner(DirectoryEntry* e, uint32_t requesting_node, uint64_t ts);

    // Remote messaging latency
    double send_remote_invalidate(uint32_t target, uint64_t addr, uint64_t ts);
    double send_remote_downgrade(uint32_t target, uint64_t addr, uint64_t ts);
    double calculate_coherency_msg_latency(uint32_t target, uint64_t ts);

    DirectoryEntry* get_or_create_entry(uint64_t addr);
    double calculate_contention_latency(uint32_t head_id, uint64_t ts) const;
};

#endif // CXLMEMSIM_COHERENCY_ENGINE_H
