/*
 * CXLMemSim DCD/GFAM models
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 * Copyright 2025 Regents of the University of California
 * UC Santa Cruz Sluglab.
 */

#include "dcd_gfam.h"

#include <algorithm>
#include <limits>

namespace {

bool ranges_overlap(uint64_t a_base, uint64_t a_size, uint64_t b_base, uint64_t b_size) {
    if (a_size == 0 || b_size == 0) {
        return false;
    }
    if (a_base <= b_base) {
        return b_base - a_base < a_size;
    }
    return a_base - b_base < b_size;
}

bool range_contains(uint64_t outer_base, uint64_t outer_size, uint64_t inner_base, uint64_t inner_size) {
    if (inner_size == 0 || inner_size > outer_size || inner_base < outer_base) {
        return false;
    }
    return inner_base - outer_base <= outer_size - inner_size;
}

} // namespace

DynamicCapacityDevice::DynamicCapacityDevice(uint64_t total_capacity_bytes, uint64_t granularity_bytes)
    : total_capacity_bytes_(total_capacity_bytes), granularity_bytes_(std::max<uint64_t>(1, granularity_bytes)) {}

uint64_t DynamicCapacityDevice::align_up(uint64_t value) const {
    if (value == 0) {
        return 0;
    }
    uint64_t remainder = value % granularity_bytes_;
    if (remainder == 0) {
        return value;
    }
    if (value > std::numeric_limits<uint64_t>::max() - (granularity_bytes_ - remainder)) {
        return 0;
    }
    return value + granularity_bytes_ - remainder;
}

bool DynamicCapacityDevice::range_within_device(uint64_t base, uint64_t size) const {
    return size > 0 && base < total_capacity_bytes_ && size <= total_capacity_bytes_ - base;
}

bool DynamicCapacityDevice::range_overlaps_active(uint64_t base, uint64_t size) const {
    return std::any_of(extents_.begin(), extents_.end(), [base, size](const DCDExtent &extent) {
        return extent.active && ranges_overlap(base, size, extent.base, extent.size);
    });
}

std::optional<uint64_t> DynamicCapacityDevice::find_free_base(uint64_t size) const {
    std::vector<DCDExtent> active;
    active.reserve(extents_.size());
    for (const auto &extent : extents_) {
        if (extent.active) {
            active.push_back(extent);
        }
    }
    std::sort(active.begin(), active.end(),
              [](const DCDExtent &lhs, const DCDExtent &rhs) { return lhs.base < rhs.base; });

    uint64_t candidate = 0;
    for (const auto &extent : active) {
        if (candidate + size <= extent.base) {
            return candidate;
        }
        if (extent.base > std::numeric_limits<uint64_t>::max() - extent.size) {
            return std::nullopt;
        }
        candidate = align_up(extent.base + extent.size);
    }

    if (range_within_device(candidate, size)) {
        return candidate;
    }
    return std::nullopt;
}

DCDAllocationResult DynamicCapacityDevice::add_capacity(uint64_t requested_base, uint64_t size, uint64_t tag,
                                                        uint64_t timestamp) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    allocation_requests_++;

    uint64_t aligned_size = size == total_capacity_bytes_ ? size : align_up(size);
    if (aligned_size == 0 || aligned_size > total_capacity_bytes_) {
        failed_requests_++;
        return {DCDStatus::INVALID_REQUEST, 0, 0, tag};
    }

    uint64_t base = requested_base;
    if (requested_base == std::numeric_limits<uint64_t>::max()) {
        auto free_base = find_free_base(aligned_size);
        if (!free_base.has_value()) {
            failed_requests_++;
            return {DCDStatus::OUT_OF_CAPACITY, 0, aligned_size, tag};
        }
        base = *free_base;
    } else if (base % granularity_bytes_ != 0) {
        failed_requests_++;
        return {DCDStatus::INVALID_REQUEST, requested_base, aligned_size, tag};
    }

    if (!range_within_device(base, aligned_size)) {
        failed_requests_++;
        return {DCDStatus::OUT_OF_CAPACITY, base, aligned_size, tag};
    }
    if (range_overlaps_active(base, aligned_size)) {
        failed_requests_++;
        return {DCDStatus::OVERLAP, base, aligned_size, tag};
    }

    uint64_t extent_tag = tag == 0 ? next_tag_++ : tag;
    extents_.push_back({base, aligned_size, extent_tag, timestamp, 0, true});
    return {DCDStatus::OK, base, aligned_size, extent_tag};
}

DCDStatus DynamicCapacityDevice::release_capacity(uint64_t base, uint64_t size, uint64_t tag, uint64_t timestamp) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    release_requests_++;

    uint64_t aligned_size = size == total_capacity_bytes_ ? size : align_up(size);
    if (!range_within_device(base, aligned_size)) {
        failed_requests_++;
        return DCDStatus::INVALID_REQUEST;
    }

    for (auto &extent : extents_) {
        bool tag_matches = tag == 0 || extent.tag == tag;
        if (extent.active && tag_matches && extent.base == base && extent.size == aligned_size) {
            extent.active = false;
            extent.released_timestamp = timestamp;
            return DCDStatus::OK;
        }
    }

    failed_requests_++;
    return DCDStatus::NOT_FOUND;
}

bool DynamicCapacityDevice::is_allocated(uint64_t addr, uint64_t size) const {
    return find_extent(addr, size).has_value();
}

std::optional<DCDExtent> DynamicCapacityDevice::find_extent(uint64_t addr, uint64_t size) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (size == 0 || addr >= total_capacity_bytes_ || size > total_capacity_bytes_ - addr) {
        return std::nullopt;
    }

    for (const auto &extent : extents_) {
        if (extent.active && range_contains(extent.base, extent.size, addr, size)) {
            return extent;
        }
    }
    return std::nullopt;
}

std::vector<DCDExtent> DynamicCapacityDevice::active_extents() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<DCDExtent> active;
    for (const auto &extent : extents_) {
        if (extent.active) {
            active.push_back(extent);
        }
    }
    std::sort(active.begin(), active.end(),
              [](const DCDExtent &lhs, const DCDExtent &rhs) { return lhs.base < rhs.base; });
    return active;
}

DCDStats DynamicCapacityDevice::stats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    uint64_t allocated = 0;
    uint64_t active_count = 0;
    for (const auto &extent : extents_) {
        if (extent.active) {
            allocated += extent.size;
            active_count++;
        }
    }

    return {total_capacity_bytes_,
            allocated,
            total_capacity_bytes_ >= allocated ? total_capacity_bytes_ - allocated : 0,
            active_count,
            allocation_requests_,
            release_requests_,
            failed_requests_};
}

GFAMDevice::GFAMDevice(DynamicCapacityDevice *dcd, double fabric_latency_ns, double bandwidth_gbps)
    : dcd_(dcd), fabric_latency_ns_(fabric_latency_ns), bandwidth_gbps_(std::max(1.0, bandwidth_gbps)) {}

void GFAMDevice::register_host(uint32_t host_id, const std::string &label) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto &host = hosts_[host_id];
    host.host_id = host_id;
    host.label = label;
    host.active = true;
    rebalance_bandwidth_locked();
}

void GFAMDevice::unregister_host(uint32_t host_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = hosts_.find(host_id);
    if (it != hosts_.end()) {
        it->second.active = false;
    }
    mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(),
                                   [host_id](const GFAMMapping &mapping) { return mapping.host_id == host_id; }),
                    mappings_.end());
    rebalance_bandwidth_locked();
}

DCDStatus GFAMDevice::grant_access(uint32_t host_id, uint64_t base, uint64_t size, uint32_t permissions) {
    if (!dcd_ || !dcd_->is_allocated(base, size) || permissions == 0) {
        return DCDStatus::INVALID_REQUEST;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto host_it = hosts_.find(host_id);
    if (host_it == hosts_.end() || !host_it->second.active) {
        return DCDStatus::ACCESS_DENIED;
    }

    for (auto &mapping : mappings_) {
        if (mapping.host_id == host_id && mapping.base == base && mapping.size == size) {
            mapping.permissions = permissions;
            return DCDStatus::OK;
        }
    }

    mappings_.push_back({host_id, base, size, permissions});
    return DCDStatus::OK;
}

DCDStatus GFAMDevice::revoke_access(uint32_t host_id, uint64_t base, uint64_t size) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto old_size = mappings_.size();
    mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(),
                                   [host_id, base, size](const GFAMMapping &mapping) {
                                       return mapping.host_id == host_id && mapping.base == base &&
                                              mapping.size == size;
                                   }),
                    mappings_.end());

    if (mappings_.size() == old_size) {
        return DCDStatus::NOT_FOUND;
    }
    return DCDStatus::OK;
}

void GFAMDevice::revoke_extent(uint64_t base, uint64_t size) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(),
                                   [base, size](const GFAMMapping &mapping) {
                                       return mapping.base == base && mapping.size == size;
                                   }),
                    mappings_.end());
}

bool GFAMDevice::mapping_covers(const GFAMMapping &mapping, uint64_t addr, uint64_t size) const {
    return range_contains(mapping.base, mapping.size, addr, size);
}

uint32_t GFAMDevice::required_permissions(bool is_write, bool is_atomic) const {
    if (is_atomic) {
        return DCD_PERM_ATOMIC | DCD_PERM_READ | DCD_PERM_WRITE;
    }
    return is_write ? DCD_PERM_WRITE : DCD_PERM_READ;
}

bool GFAMDevice::check_access(uint32_t host_id, uint64_t addr, uint64_t size, bool is_write, bool is_atomic) const {
    if (!dcd_ || !dcd_->is_allocated(addr, size)) {
        return false;
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto host_it = hosts_.find(host_id);
    if (host_it == hosts_.end() || !host_it->second.active) {
        return false;
    }

    uint32_t required = required_permissions(is_write, is_atomic);
    for (const auto &mapping : mappings_) {
        if (mapping.host_id == host_id && mapping_covers(mapping, addr, size) &&
            (mapping.permissions & required) == required) {
            return true;
        }
    }
    return false;
}

double GFAMDevice::calculate_access_latency(uint32_t host_id, uint64_t size) const {
    auto host_it = hosts_.find(host_id);
    double bandwidth_share = host_it != hosts_.end() ? host_it->second.bandwidth_share : 0.0;
    if (bandwidth_share <= 0.0) {
        bandwidth_share = 1.0;
    }

    double effective_bandwidth_gbps = bandwidth_gbps_ * bandwidth_share;
    double transfer_time_ns = (static_cast<double>(size) * 8.0) / (effective_bandwidth_gbps * 1e9) * 1e9;
    double contention_penalty = bandwidth_share < 1.0 ? fabric_latency_ns_ * (1.0 / bandwidth_share - 1.0) * 0.25 : 0.0;
    return fabric_latency_ns_ + transfer_time_ns + contention_penalty;
}

GFAMAccessResult GFAMDevice::record_access(uint32_t host_id, uint64_t addr, uint64_t size, bool is_write,
                                           bool is_atomic, uint64_t timestamp) {
    (void)timestamp;

    if (!dcd_ || !dcd_->is_allocated(addr, size)) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        denied_accesses_++;
        return {false, DCDStatus::ACCESS_DENIED, 0.0};
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto host_it = hosts_.find(host_id);
    if (host_it == hosts_.end() || !host_it->second.active) {
        denied_accesses_++;
        return {false, DCDStatus::ACCESS_DENIED, 0.0};
    }

    uint32_t required = required_permissions(is_write, is_atomic);
    bool allowed = false;
    for (const auto &mapping : mappings_) {
        if (mapping.host_id == host_id && mapping_covers(mapping, addr, size) &&
            (mapping.permissions & required) == required) {
            allowed = true;
            break;
        }
    }

    if (!allowed) {
        denied_accesses_++;
        return {false, DCDStatus::ACCESS_DENIED, 0.0};
    }

    if (is_atomic) {
        host_it->second.atomics++;
    } else if (is_write) {
        host_it->second.writes++;
    } else {
        host_it->second.reads++;
    }

    double latency = calculate_access_latency(host_id, size);
    total_latency_ns_ += static_cast<uint64_t>(latency);
    total_accesses_++;
    return {true, DCDStatus::OK, latency};
}

GFAMStats GFAMDevice::stats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    uint64_t active_hosts = 0;
    uint64_t reads = 0;
    uint64_t writes = 0;
    uint64_t atomics = 0;
    for (const auto &[id, host] : hosts_) {
        (void)id;
        if (host.active) {
            active_hosts++;
        }
        reads += host.reads;
        writes += host.writes;
        atomics += host.atomics;
    }

    uint64_t shared_mappings = 0;
    for (const auto &mapping : mappings_) {
        if ((mapping.permissions & DCD_PERM_SHARED) != 0) {
            shared_mappings++;
        }
    }

    return {active_hosts,     static_cast<uint64_t>(mappings_.size()),
            shared_mappings,  reads,
            writes,           atomics,
            denied_accesses_, total_accesses_ > 0 ? static_cast<double>(total_latency_ns_) / total_accesses_ : 0.0};
}

void GFAMDevice::rebalance_bandwidth_locked() {
    uint64_t active_hosts = 0;
    for (const auto &[id, host] : hosts_) {
        (void)id;
        if (host.active) {
            active_hosts++;
        }
    }

    double share = active_hosts > 0 ? 1.0 / static_cast<double>(active_hosts) : 0.0;
    for (auto &[id, host] : hosts_) {
        (void)id;
        host.bandwidth_share = host.active ? share : 0.0;
    }
}
