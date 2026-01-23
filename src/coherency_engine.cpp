/*
 * CXLMemSim Unified MOESI Coherency Engine Implementation
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 * Copyright 2025 Regents of the University of California
 * UC Santa Cruz Sluglab.
 */

#include "coherency_engine.h"
#include "hdm_decoder.h"
#include "cxlendpoint.h"
#include <algorithm>
#include <cmath>

DirectoryEntry::DirectoryEntry()
    : cacheline_addr(0), state(MHSLDCacheState::INVALID),
      owner_node(UINT32_MAX), owner_head(UINT32_MAX),
      version(0), last_access_time(0), has_dirty_data(false) {}

CoherencyEngine::CoherencyEngine(uint32_t local_node, HDMDecoder* decoder, LogPModel* logp,
                                  uint32_t max_heads, double bandwidth_gbps)
    : local_node_id_(local_node), hdm_decoder_(decoder), logp_model_(logp),
      bandwidth_gbps_(bandwidth_gbps) {
    heads_.resize(max_heads);
    for (uint32_t i = 0; i < max_heads; i++) {
        heads_[i].head_id = i;
        heads_[i].active = false;
        heads_[i].bandwidth_share = 0.0;
    }
}

DirectoryEntry* CoherencyEngine::get_or_create_entry(uint64_t addr) {
    uint64_t cl_addr = addr & ~(CACHELINE_SIZE - 1);

    {
        std::shared_lock<std::shared_mutex> lock(directory_mutex_);
        auto it = directory_.find(cl_addr);
        if (it != directory_.end()) {
            return it->second.get();
        }
    }

    std::unique_lock<std::shared_mutex> lock(directory_mutex_);
    auto& entry = directory_[cl_addr];
    if (!entry) {
        entry = std::make_unique<DirectoryEntry>();
        entry->cacheline_addr = cl_addr;
    }
    return entry.get();
}

double CoherencyEngine::calculate_coherency_msg_latency(uint32_t target, uint64_t ts) {
    double latency = 0.0;

    // Fabric link traversal if available
    auto it = fabric_links_.find(target);
    if (it != fabric_links_.end() && it->second) {
        latency += it->second->calculate_traversal_latency(ts, CACHELINE_SIZE);
    }

    // LogP network model latency
    if (logp_model_ && target != local_node_id_) {
        latency += logp_model_->message_latency(ts, target);
    }

    return latency;
}

double CoherencyEngine::calculate_contention_latency(uint32_t head_id, uint64_t ts) const {
    if (head_id >= heads_.size() || !heads_[head_id].active) return 0.0;

    uint32_t contending_heads = 0;
    for (const auto& h : heads_) {
        if (h.active && (h.total_reads + h.total_writes) > 0) {
            contending_heads++;
        }
    }

    if (contending_heads <= 1) return 0.0;

    double fair_share = heads_[head_id].bandwidth_share;
    if (fair_share <= 0.0) fair_share = 1.0 / contending_heads;

    double contention_factor = 0.3;
    double base_latency = 100.0; // Base device latency
    double additional = base_latency * (1.0 / fair_share - 1.0) * contention_factor;

    return std::min(additional, base_latency * 5.0);
}

/* ============================================================================
 * MOESI Read State Machine
 * ============================================================================ */
CoherencyResponse CoherencyEngine::process_read(const CoherencyRequest& req) {
    DirectoryEntry* entry = get_or_create_entry(req.addr);
    std::lock_guard<std::mutex> lock(entry->lock);

    double latency = 0.0;
    MHSLDCacheState new_state = MHSLDCacheState::SHARED;
    uint32_t data_source = local_node_id_;
    bool is_remote_access = (req.requesting_node != local_node_id_);

    entry->last_access_time = req.timestamp;

    // Check if requester already has access
    if (entry->owner_node == req.requesting_node &&
        (entry->state == MHSLDCacheState::EXCLUSIVE ||
         entry->state == MHSLDCacheState::MODIFIED ||
         entry->state == MHSLDCacheState::OWNED)) {
        // Local hit
        latency = calculate_contention_latency(req.requesting_head, req.timestamp);
        new_state = entry->state;
        goto done;
    }

    if (entry->sharer_nodes.count(req.requesting_node) > 0) {
        // Already a sharer - hit
        latency = calculate_contention_latency(req.requesting_head, req.timestamp);
        new_state = MHSLDCacheState::SHARED;
        goto done;
    }

    // Need to acquire shared access
    switch (entry->state) {
        case MHSLDCacheState::INVALID:
            // First access - fetch from memory
            entry->sharer_nodes.insert(req.requesting_node);
            entry->state = MHSLDCacheState::SHARED;
            entry->owner_node = req.requesting_node;
            entry->owner_head = req.requesting_head;
            new_state = MHSLDCacheState::SHARED;
            // Base memory access latency (no coherency overhead)
            break;

        case MHSLDCacheState::SHARED:
            // Add as sharer, no invalidation needed
            entry->sharer_nodes.insert(req.requesting_node);
            new_state = MHSLDCacheState::SHARED;
            break;

        case MHSLDCacheState::EXCLUSIVE:
            if (entry->owner_node == req.requesting_node) {
                // Same node, different head - local hit with contention
                latency = calculate_contention_latency(req.requesting_head, req.timestamp);
            } else {
                // Downgrade owner to SHARED
                latency = downgrade_owner(entry, req.requesting_node, req.timestamp);
                data_source = entry->owner_node;
            }
            entry->sharer_nodes.insert(req.requesting_node);
            entry->state = MHSLDCacheState::SHARED;
            new_state = MHSLDCacheState::SHARED;
            break;

        case MHSLDCacheState::MODIFIED:
            if (entry->owner_node == req.requesting_node) {
                latency = calculate_contention_latency(req.requesting_head, req.timestamp);
            } else {
                // Fetch from owner, owner transitions to OWNED
                latency = fetch_from_owner(entry, req.requesting_node, req.timestamp);
                data_source = entry->owner_node;
                entry->state = MHSLDCacheState::OWNED;
            }
            entry->sharer_nodes.insert(req.requesting_node);
            new_state = MHSLDCacheState::SHARED;
            break;

        case MHSLDCacheState::OWNED:
            if (entry->owner_node == req.requesting_node) {
                latency = calculate_contention_latency(req.requesting_head, req.timestamp);
            } else {
                // Forward data from owner
                latency = calculate_coherency_msg_latency(entry->owner_node, req.timestamp);
                data_source = entry->owner_node;
                total_coherency_messages_++;
            }
            entry->sharer_nodes.insert(req.requesting_node);
            new_state = MHSLDCacheState::SHARED;
            break;
    }

done:
    if (is_remote_access) {
        total_remote_ops_++;
    }

    latency += calculate_contention_latency(req.requesting_head, req.timestamp);
    total_ops_++;
    total_latency_ns_ += static_cast<uint64_t>(latency);

    return {latency, new_state, true, data_source};
}

/* ============================================================================
 * MOESI Write State Machine
 * ============================================================================ */
CoherencyResponse CoherencyEngine::process_write(const CoherencyRequest& req) {
    DirectoryEntry* entry = get_or_create_entry(req.addr);
    std::lock_guard<std::mutex> lock(entry->lock);

    double latency = 0.0;
    MHSLDCacheState new_state = MHSLDCacheState::MODIFIED;
    uint32_t data_source = local_node_id_;
    bool is_remote_access = (req.requesting_node != local_node_id_);

    entry->last_access_time = req.timestamp;
    entry->version++;

    // Check if requester already has exclusive/modified access
    if (entry->owner_node == req.requesting_node) {
        if (entry->state == MHSLDCacheState::EXCLUSIVE ||
            entry->state == MHSLDCacheState::MODIFIED) {
            // Local hit - just mark as modified
            entry->state = MHSLDCacheState::MODIFIED;
            entry->has_dirty_data = true;
            latency = calculate_contention_latency(req.requesting_head, req.timestamp);
            new_state = MHSLDCacheState::MODIFIED;
            goto done;
        }
        if (entry->state == MHSLDCacheState::OWNED) {
            // Need to invalidate sharers
            latency = invalidate_sharers(entry, req.requesting_node, req.timestamp);
            entry->state = MHSLDCacheState::MODIFIED;
            entry->has_dirty_data = true;
            new_state = MHSLDCacheState::MODIFIED;
            goto done;
        }
    }

    // Need to acquire exclusive access
    switch (entry->state) {
        case MHSLDCacheState::INVALID:
            // First access - no coherency needed
            break;

        case MHSLDCacheState::SHARED:
            // Invalidate all sharers
            latency = invalidate_sharers(entry, req.requesting_node, req.timestamp);
            break;

        case MHSLDCacheState::EXCLUSIVE:
            if (entry->owner_node != req.requesting_node) {
                // Invalidate current owner
                latency = calculate_coherency_msg_latency(entry->owner_node, req.timestamp);
                total_invalidations_++;
                total_coherency_messages_++;
            }
            break;

        case MHSLDCacheState::MODIFIED:
            if (entry->owner_node != req.requesting_node) {
                // Fetch + invalidate owner
                latency = calculate_coherency_msg_latency(entry->owner_node, req.timestamp);
                data_source = entry->owner_node;
                total_writebacks_++;
                total_coherency_messages_++;
            }
            break;

        case MHSLDCacheState::OWNED:
            if (entry->owner_node != req.requesting_node) {
                // Invalidate owner + sharers
                double owner_lat = calculate_coherency_msg_latency(entry->owner_node, req.timestamp);
                double sharer_lat = invalidate_sharers(entry, req.requesting_node, req.timestamp);
                latency = owner_lat + sharer_lat;
                data_source = entry->owner_node;
                total_writebacks_++;
            } else {
                latency = invalidate_sharers(entry, req.requesting_node, req.timestamp);
            }
            break;
    }

    // Transition to MODIFIED for the requesting node
    entry->owner_node = req.requesting_node;
    entry->owner_head = req.requesting_head;
    entry->state = MHSLDCacheState::MODIFIED;
    entry->sharer_nodes.clear();
    entry->has_dirty_data = true;
    new_state = MHSLDCacheState::MODIFIED;

done:
    if (is_remote_access) {
        total_remote_ops_++;
    }

    latency += calculate_contention_latency(req.requesting_head, req.timestamp);
    total_ops_++;
    total_latency_ns_ += static_cast<uint64_t>(latency);

    return {latency, new_state, true, data_source};
}

CoherencyResponse CoherencyEngine::process_atomic(const CoherencyRequest& req) {
    // Atomic requires exclusive access + serialization penalty
    auto resp = process_write(req);

    // Additional serialization overhead for atomic operations
    if (logp_model_) {
        double atomic_overhead = logp_model_->config.o_s + logp_model_->config.o_r;
        resp.latency_ns += atomic_overhead;
    }

    return resp;
}

/* ============================================================================
 * Coherency Actions
 * ============================================================================ */

double CoherencyEngine::invalidate_sharers(DirectoryEntry* e, uint32_t except_node, uint64_t ts) {
    double max_latency = 0.0;
    double accumulated_gap = 0.0;

    std::set<uint32_t> to_invalidate;
    for (uint32_t sharer : e->sharer_nodes) {
        if (sharer != except_node) {
            to_invalidate.insert(sharer);
        }
    }

    for (uint32_t sharer : to_invalidate) {
        double inv_latency = accumulated_gap + calculate_coherency_msg_latency(sharer, ts);
        max_latency = std::max(max_latency, inv_latency);

        if (logp_model_) {
            accumulated_gap += logp_model_->config.g;
        }

        total_invalidations_++;
        total_coherency_messages_++;
    }

    e->sharer_nodes.clear();
    return max_latency;
}

double CoherencyEngine::downgrade_owner(DirectoryEntry* e, uint32_t requesting_node, uint64_t ts) {
    if (e->owner_node == UINT32_MAX || e->owner_node == requesting_node) return 0.0;

    double latency = calculate_coherency_msg_latency(e->owner_node, ts);

    if (e->state == MHSLDCacheState::MODIFIED) {
        e->state = MHSLDCacheState::OWNED;
        e->has_dirty_data = true;
    } else if (e->state == MHSLDCacheState::EXCLUSIVE) {
        e->sharer_nodes.insert(e->owner_node);
        e->state = MHSLDCacheState::SHARED;
        e->owner_node = UINT32_MAX;
        e->owner_head = UINT32_MAX;
    }

    total_downgrades_++;
    total_coherency_messages_++;
    return latency;
}

double CoherencyEngine::fetch_from_owner(DirectoryEntry* e, uint32_t requesting_node, uint64_t ts) {
    if (e->owner_node == UINT32_MAX) return 0.0;

    double latency = calculate_coherency_msg_latency(e->owner_node, ts);
    total_coherency_messages_++;
    return latency;
}

double CoherencyEngine::transition_to_shared(DirectoryEntry* e, uint32_t node, uint32_t head, uint64_t ts) {
    e->sharer_nodes.insert(node);
    if (e->state == MHSLDCacheState::INVALID) {
        e->state = MHSLDCacheState::SHARED;
        e->owner_node = node;
        e->owner_head = head;
    }
    return 0.0;
}

double CoherencyEngine::transition_to_exclusive(DirectoryEntry* e, uint32_t node, uint32_t head, uint64_t ts) {
    double latency = invalidate_sharers(e, node, ts);
    e->owner_node = node;
    e->owner_head = head;
    e->state = MHSLDCacheState::EXCLUSIVE;
    e->sharer_nodes.clear();
    return latency;
}

double CoherencyEngine::transition_to_modified(DirectoryEntry* e, uint32_t node, uint32_t head, uint64_t ts) {
    double latency = transition_to_exclusive(e, node, head, ts);
    e->state = MHSLDCacheState::MODIFIED;
    e->has_dirty_data = true;
    return latency;
}

/* ============================================================================
 * Remote Message Handlers
 * ============================================================================ */

void CoherencyEngine::handle_remote_invalidate(uint64_t addr, uint32_t from_node) {
    uint64_t cl_addr = addr & ~(CACHELINE_SIZE - 1);

    std::shared_lock<std::shared_mutex> dir_lock(directory_mutex_);
    auto it = directory_.find(cl_addr);
    if (it == directory_.end()) return;

    DirectoryEntry* entry = it->second.get();
    std::lock_guard<std::mutex> lock(entry->lock);

    // Remove local node from sharers
    entry->sharer_nodes.erase(local_node_id_);

    // If we were the owner, transition to INVALID
    if (entry->owner_node == local_node_id_) {
        if (entry->state == MHSLDCacheState::MODIFIED) {
            entry->has_dirty_data = true;
            total_writebacks_++;
        }
        entry->owner_node = UINT32_MAX;
        entry->owner_head = UINT32_MAX;
        entry->state = MHSLDCacheState::INVALID;
    }

    total_invalidations_++;
}

void CoherencyEngine::handle_remote_downgrade(uint64_t addr, uint32_t from_node) {
    uint64_t cl_addr = addr & ~(CACHELINE_SIZE - 1);

    std::shared_lock<std::shared_mutex> dir_lock(directory_mutex_);
    auto it = directory_.find(cl_addr);
    if (it == directory_.end()) return;

    DirectoryEntry* entry = it->second.get();
    std::lock_guard<std::mutex> lock(entry->lock);

    if (entry->owner_node == local_node_id_) {
        if (entry->state == MHSLDCacheState::MODIFIED) {
            entry->state = MHSLDCacheState::OWNED;
        } else if (entry->state == MHSLDCacheState::EXCLUSIVE) {
            entry->state = MHSLDCacheState::SHARED;
            entry->sharer_nodes.insert(local_node_id_);
        }
    }

    total_downgrades_++;
}

void CoherencyEngine::handle_remote_writeback(uint64_t addr, uint32_t from_node, const uint8_t* data) {
    uint64_t cl_addr = addr & ~(CACHELINE_SIZE - 1);

    std::shared_lock<std::shared_mutex> dir_lock(directory_mutex_);
    auto it = directory_.find(cl_addr);
    if (it == directory_.end()) return;

    DirectoryEntry* entry = it->second.get();
    std::lock_guard<std::mutex> lock(entry->lock);

    if (entry->owner_node == from_node) {
        entry->has_dirty_data = false;
        entry->owner_node = UINT32_MAX;
        entry->owner_head = UINT32_MAX;
        entry->state = MHSLDCacheState::INVALID;
        entry->sharer_nodes.erase(from_node);
    }

    total_writebacks_++;
}

/* ============================================================================
 * Head Management
 * ============================================================================ */

void CoherencyEngine::activate_head(uint32_t head_id, uint64_t capacity) {
    if (head_id >= heads_.size()) return;

    heads_[head_id].active = true;
    heads_[head_id].allocated_capacity = capacity;
    heads_[head_id].used_capacity = 0;

    // Rebalance bandwidth
    uint32_t active_count = 0;
    for (const auto& h : heads_) {
        if (h.active) active_count++;
    }
    for (auto& h : heads_) {
        if (h.active) {
            h.bandwidth_share = 1.0 / active_count;
        }
    }
}

void CoherencyEngine::deactivate_head(uint32_t head_id) {
    if (head_id >= heads_.size()) return;
    heads_[head_id].active = false;
    heads_[head_id].bandwidth_share = 0.0;
}

void CoherencyEngine::register_fabric_link(uint32_t node_id, FabricLink* link) {
    fabric_links_[node_id] = link;
}

void CoherencyEngine::set_rdma_transport(DistributedRDMATransport* rdma) {
    rdma_transport_ = rdma;
}

void CoherencyEngine::set_msg_manager(DistributedMessageManager* msg) {
    msg_manager_ = msg;
}

CoherencyEngine::Stats CoherencyEngine::get_stats() const {
    Stats stats;
    stats.coherency_messages = total_coherency_messages_.load();
    stats.invalidations = total_invalidations_.load();
    stats.downgrades = total_downgrades_.load();
    stats.writebacks = total_writebacks_.load();
    stats.remote_ops = total_remote_ops_.load();

    uint64_t ops = total_ops_.load();
    uint64_t lat = total_latency_ns_.load();
    stats.avg_coherency_latency = ops > 0 ? static_cast<double>(lat) / ops : 0.0;

    return stats;
}

double CoherencyEngine::send_remote_invalidate(uint32_t target, uint64_t addr, uint64_t ts) {
    return calculate_coherency_msg_latency(target, ts);
}

double CoherencyEngine::send_remote_downgrade(uint32_t target, uint64_t addr, uint64_t ts) {
    return calculate_coherency_msg_latency(target, ts);
}
