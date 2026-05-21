/*
 * CXLMemSim DCD/GFAM models
 *
 * Dynamic Capacity Device (DCD): runtime tagged capacity add/release.
 * Global Fabric Attached Memory (GFAM): shared fabric memory access control
 * and contention/latency accounting across host ports.
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 * Copyright 2026 Regents of the University of California
 * UC Santa Cruz Sluglab.
 */

#ifndef CXLMEMSIM_DCD_GFAM_H
#define CXLMEMSIM_DCD_GFAM_H

#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

enum DCDPermission : uint32_t {
    DCD_PERM_READ = 1u << 0,
    DCD_PERM_WRITE = 1u << 1,
    DCD_PERM_ATOMIC = 1u << 2,
    DCD_PERM_SHARED = 1u << 3,
    DCD_PERM_ALL = DCD_PERM_READ | DCD_PERM_WRITE | DCD_PERM_ATOMIC | DCD_PERM_SHARED,
};

enum class DCDStatus : uint8_t {
    OK = 0,
    OUT_OF_CAPACITY = 1,
    OVERLAP = 2,
    NOT_FOUND = 3,
    INVALID_REQUEST = 4,
    ACCESS_DENIED = 5,
};

struct DCDExtent {
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t tag = 0;
    uint64_t created_timestamp = 0;
    uint64_t released_timestamp = 0;
    bool active = true;
};

struct DCDAllocationResult {
    DCDStatus status = DCDStatus::INVALID_REQUEST;
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t tag = 0;
};

struct DCDStats {
    uint64_t total_capacity = 0;
    uint64_t allocated_capacity = 0;
    uint64_t free_capacity = 0;
    uint64_t active_extents = 0;
    uint64_t allocation_requests = 0;
    uint64_t release_requests = 0;
    uint64_t failed_requests = 0;
};

class DynamicCapacityDevice {
public:
    DynamicCapacityDevice(uint64_t total_capacity_bytes, uint64_t granularity_bytes);

    DCDAllocationResult add_capacity(uint64_t requested_base, uint64_t size, uint64_t tag, uint64_t timestamp = 0);
    DCDStatus release_capacity(uint64_t base, uint64_t size, uint64_t tag, uint64_t timestamp = 0);

    bool is_allocated(uint64_t addr, uint64_t size) const;
    std::optional<DCDExtent> find_extent(uint64_t addr, uint64_t size) const;
    std::vector<DCDExtent> active_extents() const;
    DCDStats stats() const;

    uint64_t total_capacity() const { return total_capacity_bytes_; }
    uint64_t granularity() const { return granularity_bytes_; }

private:
    uint64_t total_capacity_bytes_;
    uint64_t granularity_bytes_;
    uint64_t next_tag_ = 1;

    std::vector<DCDExtent> extents_;
    mutable std::shared_mutex mutex_;

    uint64_t allocation_requests_ = 0;
    uint64_t release_requests_ = 0;
    uint64_t failed_requests_ = 0;

    uint64_t align_up(uint64_t value) const;
    bool range_within_device(uint64_t base, uint64_t size) const;
    bool range_overlaps_active(uint64_t base, uint64_t size) const;
    std::optional<uint64_t> find_free_base(uint64_t size) const;
};

struct GFAMHost {
    uint32_t host_id = 0;
    std::string label;
    bool active = true;
    double bandwidth_share = 0.0;
    uint64_t reads = 0;
    uint64_t writes = 0;
    uint64_t atomics = 0;
};

struct GFAMMapping {
    uint32_t host_id = 0;
    uint64_t base = 0;
    uint64_t size = 0;
    uint32_t permissions = 0;
};

struct GFAMAccessResult {
    bool allowed = false;
    DCDStatus status = DCDStatus::ACCESS_DENIED;
    double latency_ns = 0.0;
};

struct GFAMStats {
    uint64_t hosts = 0;
    uint64_t mappings = 0;
    uint64_t shared_mappings = 0;
    uint64_t read_ops = 0;
    uint64_t write_ops = 0;
    uint64_t atomic_ops = 0;
    uint64_t denied_accesses = 0;
    double avg_access_latency_ns = 0.0;
};

class GFAMDevice {
public:
    GFAMDevice(DynamicCapacityDevice *dcd, double fabric_latency_ns, double bandwidth_gbps);

    void register_host(uint32_t host_id, const std::string &label = "");
    void unregister_host(uint32_t host_id);

    DCDStatus grant_access(uint32_t host_id, uint64_t base, uint64_t size, uint32_t permissions);
    DCDStatus revoke_access(uint32_t host_id, uint64_t base, uint64_t size);

    GFAMAccessResult record_access(uint32_t host_id, uint64_t addr, uint64_t size, bool is_write, bool is_atomic,
                                   uint64_t timestamp);
    bool check_access(uint32_t host_id, uint64_t addr, uint64_t size, bool is_write, bool is_atomic) const;

    GFAMStats stats() const;

private:
    DynamicCapacityDevice *dcd_;
    double fabric_latency_ns_;
    double bandwidth_gbps_;

    std::unordered_map<uint32_t, GFAMHost> hosts_;
    std::vector<GFAMMapping> mappings_;
    mutable std::shared_mutex mutex_;

    uint64_t denied_accesses_ = 0;
    uint64_t total_latency_ns_ = 0;
    uint64_t total_accesses_ = 0;

    bool mapping_covers(const GFAMMapping &mapping, uint64_t addr, uint64_t size) const;
    uint32_t required_permissions(bool is_write, bool is_atomic) const;
    double calculate_access_latency(uint32_t host_id, uint64_t size) const;
    void rebalance_bandwidth_locked();
};

#endif // CXLMEMSIM_DCD_GFAM_H
