/*
 * CXLMemSim controller
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#ifndef CXLMEMSIM_CXLCONTROLLER_H
#define CXLMEMSIM_CXLCONTROLLER_H

#include "coherency_engine.h"
#include "cxlendpoint.h"
#include "dcd_gfam.h"
#include "hdm_decoder.h"
#include "lbr.h"
#include <array>
#include <format>
#include <functional>
#include <memory>
#include <queue>
#include <shared_mutex>
#include <string_view>
class Monitors;
struct mem_stats;
struct proc_info;
struct lbr;
struct cntr;
struct TCPCalibrationResult;
enum page_type { CACHELINE, PAGE, HUGEPAGE_2M, HUGEPAGE_1G };

class Policy {
public:
    virtual ~Policy() = default;
    Policy() = default;
    virtual int compute_once(CXLController *) = 0; // reader writer
};

class AllocationPolicy : public Policy {
public:
    AllocationPolicy();
    virtual ~AllocationPolicy() = default;
    int compute_once(CXLController *controller) override { return 0; };
};

class MigrationPolicy : public Policy {
public:
    MigrationPolicy() = default;
    virtual ~MigrationPolicy() = default;

    // compute_once
    // Basic compute_once method, determines whether migration should be executed
    int compute_once(CXLController *controller) override {
        auto migration_list = get_migration_list(controller);
        return migration_list.empty() ? 0 : 1;
    }

    // Get the list of addresses that need migration
    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> migration_list;
        // Base class provides empty implementation
        return migration_list;
    }
    // Determine if a specific address should be migrated
    virtual bool should_migrate(uint64_t addr, uint64_t timestamp, int current_device) { return false; }

    // Select the best target device for a given address
    virtual int select_target_device(uint64_t addr, int current_device, CXLController *controller) {
        return -1; // -1
                   // -1 means no migration
    }
};

// need to give a timeout and will be added latency later,
class PagingPolicy : public Policy {
public:
    PagingPolicy();
    int compute_once(CXLController *) override { return 0; };
    // paging related
    virtual uint64_t check_page_table_walk(uint64_t virt_addr, uint64_t phys_addr, bool is_remote, page_type pt) {
        return 0;
    }
};

class CachingPolicy : public Policy {
public:
    CachingPolicy();
    virtual ~CachingPolicy() = default;
    int compute_once(CXLController *) override { return 0; };

    // Determine if data should be cached
    virtual bool should_cache(uint64_t addr, uint64_t timestamp) {
        return true; //
                     // Default behavior, can be overridden by subclasses
    }

    // Determine if backward invalidation should be performed
    virtual bool should_invalidate(uint64_t addr, uint64_t timestamp) {
        return false; //
                      // Default behavior, can be overridden by subclasses
    }

    // Get the list of addresses that need invalidation
    virtual std::vector<uint64_t> get_invalidation_list(CXLController *controller) {
        return {}; //
                   // Default behavior, can be overridden by subclasses
    }
};

struct LRUCacheEntry {
    uint64_t key; // Cache key (address)
    uint64_t value; // Cache value
    uint64_t timestamp; // Last access timestamp
};
// LRU
class LRUCache {
public:
    int capacity;
    std::unordered_map<uint64_t, LRUCacheEntry> cache;
    std::list<uint64_t> lru_list; // LRU
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> lru_map; // O(1)
    mutable std::shared_mutex rwmutex_;
    explicit LRUCache(int size) : capacity(size) {}

    std::optional<uint64_t> get(uint64_t key, uint64_t timestamp) {
        {
            std::shared_lock<std::shared_mutex> readLock(rwmutex_);
            auto it = cache.find(key);
            if (it == cache.end()) {
                return std::nullopt;
            }
        }

        std::unique_lock<std::shared_mutex> writeLock(rwmutex_);
        auto it = cache.find(key);
        if (it == cache.end()) {
            return std::nullopt;
        }
        lru_list.erase(lru_map[key]);
        lru_list.push_front(key);
        lru_map[key] = lru_list.begin();
        it->second.timestamp = timestamp;
        return it->second.value;
    }

    void put(uint64_t key, uint64_t value, uint64_t timestamp) {
        std::unique_lock<std::shared_mutex> writeLock(rwmutex_);

        if (cache.find(key) != cache.end()) {
            lru_list.erase(lru_map[key]);
        } else if (cache.size() >= capacity) {
            uint64_t lru_key = lru_list.back();
            lru_list.pop_back();
            lru_map.erase(lru_key);
            cache.erase(lru_key);
        }

        lru_list.push_front(key);
        lru_map[key] = lru_list.begin();
        cache[key] = {key, value, timestamp};
    }

    std::tuple<int, int> get_stats() const {
        std::shared_lock<std::shared_mutex> readLock(rwmutex_);
        return {cache.size(), capacity};
    }
    void clear() {
        std::unique_lock<std::shared_mutex> writeLock(rwmutex_);
        cache.clear();
        lru_list.clear();
        lru_map.clear();
    }

    size_t size() const {
        std::shared_lock<std::shared_mutex> readLock(rwmutex_);
        return cache.size();
    }
    bool remove(uint64_t key) {
        std::unique_lock<std::shared_mutex> writeLock(rwmutex_);
        auto it = cache.find(key);
        if (it == cache.end()) {
            return false;
        }
        // LRU
        lru_list.erase(lru_map[key]);
        lru_map.erase(key);
        cache.erase(key);
        return true;
    }
    // LRU
    bool contains(uint64_t key) const {
        std::shared_lock<std::shared_mutex> readLock(rwmutex_);
        return cache.find(key) != cache.end();
    }

    // LRU
    std::optional<uint64_t> peek(uint64_t key) const {
        std::shared_lock<std::shared_mutex> readLock(rwmutex_);
        auto it = cache.find(key);
        if (it == cache.end()) {
            return std::nullopt;
        }
        return it->second.value;
    }
};

class CXLController : public CXLSwitch {
public:
    std::vector<CXLMemExpander *> cur_expanders{};
    int capacity; // GB
    AllocationPolicy *allocation_policy{};
    MigrationPolicy *migration_policy{};
    PagingPolicy *paging_policy{};
    CachingPolicy *caching_policy{};
    CXLCounter counter;
    std::map<uint64_t, occupation_info> occupation;
    page_type page_type_; // percentage
    // no need for va pa map because v-indexed will not caught by us
    int num_switches = 0;
    int num_end_points = 0;
    int last_index = 0;
    uint64_t freed = 0;
    double latency_lat{};
    double bandwidth_lat{};
    double dramlatency;
    std::unordered_map<int, CXLMemExpander *> device_map;
    // ring buffer
    std::queue<lbr> ring_buffer;
    // rob info
    std::unordered_map<uint64_t, thread_info> thread_map;
    // LRU cache
    LRUCache lru_cache;

    // LogP queuing model for distributed/multi-node latency
    LogPModel logp_model;

    // MH-SLD device for multi-headed pooling/sharing (legacy, use CoherencyEngine instead)
    std::unique_ptr<MHSLDDevice> mhsld_device;

    // CXL 3.x dynamic capacity and global fabric attached memory models
    std::unique_ptr<DynamicCapacityDevice> dcd_device;
    std::unique_ptr<GFAMDevice> gfam_device;

    // Distributed topology support
    uint32_t local_node_id_ = 0;
    std::unique_ptr<HDMDecoder> hdm_decoder_;
    std::unique_ptr<CoherencyEngine> coherency_;
    std::vector<RemoteCXLExpander *> remote_expanders_;

    explicit CXLController(std::array<Policy *, 4> p, int capacity, page_type page_type_, int epoch,
                           double dramlatency);
    void construct_topo(std::string_view newick_tree);
    void insert_end_point(CXLMemExpander *end_point);
    std::vector<std::string> tokenize(const std::string_view &s);
    std::tuple<double, std::vector<uint64_t>> calculate_congestion() override;
    void set_epoch(int epoch) override;
    std::vector<std::tuple<uint64_t, uint64_t>> get_access(uint64_t timestamp) override;
    double calculate_latency(const std::vector<std::tuple<uint64_t, uint64_t>> &elem,
                             double dramlatency) override; // traverse the tree to calculate the latency
    double calculate_bandwidth(const std::vector<std::tuple<uint64_t, uint64_t>> &elem) override;
    void insert_one(thread_info &t_info, lbr &lbr);
    int insert(uint64_t timestamp, uint64_t tid, lbr lbrs[32], cntr counters[32]);
    int insert(uint64_t timestamp, uint64_t tid, uint64_t phys_addr, uint64_t virt_addr, int index) override;
    void delete_entry(uint64_t addr, uint64_t length) override;
    void perform_migration();
    std::optional<uint64_t> access_cache(uint64_t addr, uint64_t timestamp) { return lru_cache.get(addr, timestamp); }

    void update_cache(uint64_t addr, uint64_t value, uint64_t timestamp) { lru_cache.put(addr, value, timestamp); }
    void perform_back_invalidation();
    void invalidate_in_expanders(uint64_t addr);
    void invalidate_in_switch(CXLSwitch *switch_, uint64_t addr);

    // LogP model configuration and access
    void configure_logp(const LogPConfig &config);
    void calibrate_logp_from_tcp(const struct TCPCalibrationResult &result);
    double calculate_logp_latency(uint32_t src_node, uint32_t dst_node, uint64_t timestamp);
    double calculate_logp_broadcast_latency();

    // MH-SLD pooling/sharing
    void enable_mhsld(uint32_t num_heads, double bandwidth_gbps);
    double mhsld_read(uint32_t head_id, uint64_t addr, uint64_t timestamp);
    double mhsld_write(uint32_t head_id, uint64_t addr, uint64_t timestamp);
    double mhsld_atomic(uint32_t head_id, uint64_t addr, uint64_t timestamp);
    MHSLDDevice::Stats get_mhsld_stats() const;

    // DCD / GFAM support
    void enable_dcd(uint64_t total_capacity_bytes, uint64_t granularity_bytes, uint64_t initial_capacity_bytes = 0,
                    uint64_t initial_tag = 1);
    bool dcd_enabled() const { return dcd_device != nullptr; }
    DCDAllocationResult dcd_add_capacity(uint64_t requested_base, uint64_t size, uint64_t tag, uint64_t timestamp = 0);
    DCDStatus dcd_release_capacity(uint64_t base, uint64_t size, uint64_t tag, uint64_t timestamp = 0);
    bool dcd_is_allocated(uint64_t addr, uint64_t size) const;
    DCDStats get_dcd_stats() const;

    void enable_gfam(uint32_t num_hosts, double fabric_latency_ns, double bandwidth_gbps);
    bool gfam_enabled() const { return gfam_device != nullptr; }
    DCDStatus gfam_grant_access(uint32_t host_id, uint64_t base, uint64_t size, uint32_t permissions);
    DCDStatus gfam_revoke_access(uint32_t host_id, uint64_t base, uint64_t size);
    GFAMAccessResult gfam_record_access(uint32_t host_id, uint64_t addr, uint64_t size, bool is_write, bool is_atomic,
                                        uint64_t timestamp);
    GFAMStats get_gfam_stats() const;

    // Combined latency: local access + LogP network + MH-SLD coherency
    double calculate_distributed_latency(const std::vector<std::tuple<uint64_t, uint64_t>> &elem, uint32_t head_id,
                                         uint32_t target_node);

    // Distributed topology configuration
    void configure_distributed(uint32_t local_node_id, HDMDecoderMode mode);
    RemoteCXLExpander *add_remote_endpoint(uint32_t remote_node, uint64_t base, uint64_t capacity,
                                           const FabricLinkConfig &link_cfg);
    RemoteCXLExpander *get_remote_expander(uint32_t node_id);
};

// C++20 std::formatter for CXLController
template <> struct std::formatter<CXLController> {
    constexpr auto parse(std::format_parse_context &ctx) -> decltype(ctx.begin()) { return ctx.end(); }

    template <typename FormatContext>
    auto format(const CXLController &controller, FormatContext &ctx) const -> decltype(ctx.out()) {
        std::string result;

        result += "CXLController:\n";
        // iterate through the topology map
        uint64_t total_capacity = 0;

        std::function<void(const CXLSwitch *)> dfs_capacity = [&](const CXLSwitch *node) {
            if (!node)
                return;

            // Traverse expanders and sum their capacity
            for (const auto *expander : node->expanders) {
                if (expander) {
                    total_capacity += expander->capacity;
                }
            }

            // Recur for all connected switches
            for (const auto *sw : node->switches) {
                dfs_capacity(sw); // Proper recursive call
            }
        };
        dfs_capacity(&controller);

        result += std::format("Total system memory capacity: {}GB\n", total_capacity);

        result += std::format("  Page Type: {}\n", [](page_type pt) {
            switch (pt) {
            case CACHELINE:
                return "CACHELINE";
            case PAGE:
                return "PAGE";
            case HUGEPAGE_2M:
                return "HUGEPAGE_2M";
            case HUGEPAGE_1G:
                return "HUGEPAGE_1G";
            default:
                return "UNKNOWN";
            }
        }(controller.page_type_));

        result += std::format("  Global Counter:\n");
        result += std::format("    Local: {}\n", controller.counter.local.get());
        result += std::format("    Remote: {}\n", controller.counter.remote.get());
        result += std::format("    HITM: {}\n", controller.counter.hitm.get());

        result += "Topology:\n";

        std::function<void(const CXLSwitch *, int)> print_switch = [&result, &print_switch](const CXLSwitch *sw,
                                                                                            int depth) {
            std::string indent(depth * 2, ' ');

            result += std::format("{}Switch:\n", indent);
            result += std::format("{}  Events:\n", indent);
            result += std::format("{}    Load: {}\n", indent, sw->counter.load.get());
            result += std::format("{}    Store: {}\n", indent, sw->counter.store.get());
            result += std::format("{}    Conflict: {}\n", indent, sw->counter.conflict.get());

            for (const auto &child : sw->switches) {
                print_switch(child, depth + 1);
            }

            for (const auto &endpoint : sw->expanders) {
                result += std::format("{}Expander:\n", indent + "  ");
                result += std::format("{}  Events:\n", indent + "  ");
                result += std::format("{}    Load: {}\n", indent + "  ", endpoint->counter.load.get());
                result += std::format("{}    Store: {}\n", indent + "  ", endpoint->counter.store.get());
                result += std::format("{}    Migrate in: {}\n", indent + "  ", endpoint->counter.migrate_in.get());
                result += std::format("{}    Migrate out: {}\n", indent + "  ", endpoint->counter.migrate_out.get());
                result += std::format("{}    Hit Old: {}\n", indent + "  ", endpoint->counter.hit_old.get());
            }
        };

        print_switch(&controller, 0);

        result += "\nStatistics:\n";
        result += std::format("  Number of Switches: {}\n", controller.num_switches);
        result += std::format("  Number of Endpoints: {}\n", controller.num_end_points);
        result += std::format("  Number of Threads created: {}\n", controller.thread_map.size());
        result += std::format("  Memory Freed: {} bytes\n", controller.freed);

        return std::format_to(ctx.out(), "{}", result);
    }
};

extern CXLController *controller;
#endif // CXLMEMSIM_CXLCONTROLLER_H
