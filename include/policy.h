/*
 * CXLMemSim policy
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#ifndef CXLMEMSIM_POLICY_H
#define CXLMEMSIM_POLICY_H
#include "cxlcontroller.h"
#include "cxlendpoint.h"
#include "helper.h"
#include <map>
#include <random>

// Saturate Local 90% and start interleave accrodingly the remote with topology
// Say 3 remote, 2 200ns, 1 400ns, will give 40% 40% 20%
class InterleavePolicy : public AllocationPolicy {

public:
    InterleavePolicy() = default;
    int last_remote = 0;
    int all_size = 0;
    std::vector<double> percentage;
    int compute_once(CXLController *) override;
};

class NUMAPolicy : public AllocationPolicy {

public:
    NUMAPolicy() = default;
    std::vector<double> latency_scores;
    int compute_once(CXLController *) override;
};

class HeatAwareMigrationPolicy : public MigrationPolicy {
public:
    std::unordered_map<uint64_t, uint64_t> access_count;
    uint64_t hot_threshold;
    HeatAwareMigrationPolicy(uint64_t threshold = 100) : hot_threshold(threshold) {}

    void record_access(uint64_t addr) { access_count[addr]++; }

    int compute_once(CXLController *controller) override {
        for (const auto &[timestamp, info] : controller->occupation) {
            record_access(info.address);
        }

        auto migration_list = get_migration_list(controller);
        return migration_list.empty() ? 0 : 1;
    }

    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> to_migrate;

        int per_size;
        switch (controller->page_type_) {
        case CACHELINE:
            per_size = 64;
            break;
        case PAGE:
            per_size = 4096;
            break;
        case HUGEPAGE_2M:
            per_size = 2 * 1024 * 1024;
            break;
        case HUGEPAGE_1G:
            per_size = 1024 * 1024 * 1024;
            break;
        };

        for (const auto &[addr, count] : access_count) {
            if (count > hot_threshold) {
                to_migrate.emplace_back(addr, per_size);
            }
        }
        return to_migrate;
    }
};

class HugePagePolicy : public PagingPolicy {
public:
    uint64_t ptw_base_latency_local;
    uint64_t ptw_base_latency_remote;

    std::unordered_map<uint64_t, uint64_t> va_pa_cache;

    // TLB
    struct TLBCache {
        std::list<uint64_t> entries; // LRU
        std::unordered_map<uint64_t, std::list<uint64_t>::iterator> index;
        size_t capacity; // TLB

        TLBCache(size_t size) : capacity(size) {}

        bool lookup(uint64_t page_addr) {
            auto it = index.find(page_addr);
            if (it != index.end()) {

                entries.erase(it->second);
                entries.push_front(page_addr);
                it->second = entries.begin();
                return true;
            }
            return false;
        }

        void insert(uint64_t page_addr) {

            auto it = index.find(page_addr);
            if (it != index.end()) {
                entries.erase(it->second);
                index.erase(it);
            }

            if (entries.size() >= capacity) {
                uint64_t victim = entries.back();
                entries.pop_back();
                index.erase(victim);
            }

            entries.push_front(page_addr);
            index[page_addr] = entries.begin();
        }
    };

    // TLB
    TLBCache tlb_4k; // 4KBTLB
    TLBCache tlb_2m; // 2MBTLB
    TLBCache tlb_1g; // 1GBTLB
    CXLHugePageEvent stats;
    explicit HugePagePolicy(uint64_t local_latency = 100, uint64_t remote_latency = 300)
        : ptw_base_latency_local(local_latency), ptw_base_latency_remote(remote_latency), tlb_4k(64), // 4KBTLB()
          tlb_2m(32), // 2MBTLB()
          tlb_1g(4) // 1GBTLB()
    {}

    uint64_t check_page_table_walk(uint64_t virt_addr, uint64_t phys_addr, bool is_remote,
                                   page_type page_size) override {
        uint64_t base_latency = is_remote ? ptw_base_latency_remote : ptw_base_latency_local;

        // TLB
        uint64_t page_addr;
        TLBCache *tlb;
        double ptw_reduction = 1.0;
        switch (page_size) {
        case CACHELINE:
        case PAGE:
            // 4KB
            page_addr = virt_addr & ~((1ULL << 12) - 1);
            tlb = &tlb_4k;
            ptw_reduction = 1.0; // 4
            break;

        case HUGEPAGE_2M:
            // 2MB
            page_addr = virt_addr & ~((1ULL << 21) - 1);
            tlb = &tlb_2m;
            ptw_reduction = 0.75; // 25%
            break;

        case HUGEPAGE_1G:
            // 1GB
            page_addr = virt_addr & ~((1ULL << 30) - 1);
            tlb = &tlb_1g;
            ptw_reduction = 0.5; // 50%
            break;

        default:
            // 4KB
            page_addr = virt_addr & ~((1ULL << 12) - 1);
            tlb = &tlb_4k;
            ptw_reduction = 1.0;
        }

        // TLB
        bool tlb_hit = tlb->lookup(page_addr);

        switch (page_size) {
        case CACHELINE:
        case PAGE:
            if (tlb_hit)
                stats.inc_tlb_hits_4k();
            else
                stats.inc_tlb_misses_4k();
            break;
        case HUGEPAGE_2M:
            if (tlb_hit)
                stats.inc_tlb_hits_2m();
            else
                stats.inc_tlb_misses_2m();
            break;
        case HUGEPAGE_1G:
            if (tlb_hit)
                stats.inc_tlb_hits_1g();
            else
                stats.inc_tlb_misses_1g();
            break;
        }

        if (tlb_hit) {
            // TLB
            return 0;
        }

        // TLB
        // VA-PA
        auto it = va_pa_cache.find(virt_addr);
        if (it != va_pa_cache.end()) {
            // TLBTLB
            tlb->insert(page_addr);

            return base_latency * 0.2 * ptw_reduction;
        }

        stats.inc_ptw_count();

        va_pa_cache[virt_addr] = phys_addr;
        tlb->insert(page_addr);

        return base_latency * ptw_reduction;
    }

    int compute_once(CXLController *controller) override {
        std::unordered_map<uint64_t, std::vector<uint64_t>> page_groups;
        size_t potential_huge_pages = 0;

        for (const auto &[timestamp, info] : controller->occupation) {
            uint64_t addr = info.address;

            // 2MB210
            uint64_t huge_page_2m_addr = addr & ~((1ULL << 21) - 1);

            // 2MB
            page_groups[huge_page_2m_addr].push_back(addr);
        }

        for (const auto &[huge_page_addr, addresses] : page_groups) {
            // 4KB
            std::unordered_set<uint64_t> unique_pages;
            for (uint64_t addr : addresses) {
                // 4KB12
                uint64_t page_addr = addr & ~((1ULL << 12) - 1);
                unique_pages.insert(page_addr);
            }

            // 4KB
            // 2MB5124KB
            if (unique_pages.size() > 64) { // 1/82MB
                potential_huge_pages++;
            }
        }

        if (potential_huge_pages > 3) {
            // 2MB
            controller->page_type_ = HUGEPAGE_2M;
            return 1;
        }

        // 1GB
        std::unordered_map<uint64_t, size_t> gb_page_usage;

        for (const auto &[huge_page_addr, addresses] : page_groups) {
            // 1GB30
            uint64_t gb_page_addr = huge_page_addr & ~((1ULL << 30) - 1);
            gb_page_usage[gb_page_addr] += addresses.size();
        }

        // 1GB
        for (const auto &[gb_addr, usage] : gb_page_usage) {
            // 1GB2621444KB
            if (usage > 16384) { // 1/161GB
                // 1GB
                controller->page_type_ = HUGEPAGE_1G;
                return 2; // 1GB
            }
        }

        return 0;
    }

    // TLB
    std::tuple<double, double, double, uint64_t> get_stats() const {
        // TLB
        double tlb_hit_rate_4k =
            stats.get_tlb_hits_4k() + stats.get_tlb_misses_4k() > 0
                ? static_cast<double>(stats.get_tlb_hits_4k()) / (stats.get_tlb_hits_4k() + stats.get_tlb_misses_4k())
                : 0.0;

        double tlb_hit_rate_2m =
            stats.get_tlb_hits_2m() + stats.get_tlb_misses_2m() > 0
                ? static_cast<double>(stats.get_tlb_hits_2m()) / (stats.get_tlb_hits_2m() + stats.get_tlb_misses_2m())
                : 0.0;

        double tlb_hit_rate_1g =
            stats.get_tlb_hits_1g() + stats.get_tlb_misses_1g() > 0
                ? static_cast<double>(stats.get_tlb_hits_1g()) / (stats.get_tlb_hits_1g() + stats.get_tlb_misses_1g())
                : 0.0;

        return {tlb_hit_rate_4k, tlb_hit_rate_2m, tlb_hit_rate_1g, stats.ptw_count};
    }
};

class PageTableAwarePolicy : public PagingPolicy {
public:
    std::unordered_map<uint64_t, uint64_t> va_pa_cache;

    uint64_t ptw_latency_local;
    uint64_t ptw_latency_remote;
    CXLPageTableEvent cache_stats;

    uint64_t last_cleanup_timestamp;
    uint64_t cleanup_interval;

    explicit PageTableAwarePolicy(uint64_t local_latency = 100, uint64_t remote_latency = 300,
                                  uint64_t cleanup_interval = 10000000)
        : ptw_latency_local(local_latency), ptw_latency_remote(remote_latency), last_cleanup_timestamp(0),
          cleanup_interval(cleanup_interval) {}

    int compute_once(CXLController *controller) override {
        uint64_t current_timestamp = controller->last_timestamp;

        if (current_timestamp - last_cleanup_timestamp > cleanup_interval) {
            va_pa_cache.clear();
            last_cleanup_timestamp = current_timestamp;
            return 1;
        }

        double hit_rate =
            cache_stats.get_total() > 0 ? static_cast<double>(cache_stats.get_hit()) / cache_stats.get_total() : 0.0;

        if (hit_rate < 0.7 && va_pa_cache.size() < 10000) {

        } else if (hit_rate > 0.9 && va_pa_cache.size() > 1000) {

            size_t to_remove = va_pa_cache.size() / 10; // 10%

            std::vector<uint64_t> keys;
            for (const auto &[key, _] : va_pa_cache) {
                keys.push_back(key);
            }

            std::random_device rd;
            std::mt19937 gen(rd());
            std::shuffle(keys.begin(), keys.end(), gen);

            for (size_t i = 0; i < to_remove && i < keys.size(); ++i) {
                va_pa_cache.erase(keys[i]);
            }

            return 2;
        }

        return 0;
    }

    uint64_t check_page_table_walk(uint64_t virt_addr, uint64_t phys_addr, bool is_remote,
                                   page_type page_size) override {
        cache_stats.inc_total();

        if (va_pa_cache.find(virt_addr) != va_pa_cache.end()) {
            cache_stats.inc_hit();
            return 0; //
        }

        cache_stats.inc_miss();

        va_pa_cache[virt_addr] = phys_addr;

        return is_remote ? ptw_latency_remote : ptw_latency_local;
    }

    // TLB
    std::tuple<uint64_t, uint64_t, double> get_stats() const {
        return {cache_stats.get_hit(), cache_stats.get_miss(),
                cache_stats.get_total() > 0 ? static_cast<double>(cache_stats.get_hit()) / cache_stats.get_total()
                                            : 0.0};
    }
};

class FIFOPolicy : public CachingPolicy {
public:
    FIFOPolicy() = default;
    int compute_once(CXLController *) override;
    std::vector<uint64_t> get_invalidation_list(CXLController *controller) override {
        std::vector<uint64_t> to_invalidate;
        for (const auto &[timestamp, info] : controller->occupation) {
            to_invalidate.push_back(info.address);
        }
        return to_invalidate;
    };
    bool should_cache(uint64_t addr, uint64_t timestamp) override { return false; };
    bool should_invalidate(uint64_t addr, uint64_t timestamp) override { return false; };
};

class FrequencyBasedInvalidationPolicy : public CachingPolicy {
public:
    std::unordered_map<uint64_t, uint64_t> access_count;
    uint64_t access_threshold;
    uint64_t last_cleanup;
    uint64_t cleanup_interval;
    explicit FrequencyBasedInvalidationPolicy(uint64_t threshold = 100, uint64_t interval = 10000000)
        : access_threshold(threshold), last_cleanup(0), cleanup_interval(interval) {}

    bool should_cache(uint64_t addr, uint64_t timestamp) override;
    bool should_invalidate(uint64_t addr, uint64_t timestamp) override;

    std::vector<uint64_t> get_invalidation_list(CXLController *controller) override;

    int compute_once(CXLController *controller) override;
};
class FrequencyBasedMigrationPolicy : public MigrationPolicy {
private:
    std::unordered_map<uint64_t, uint64_t> access_count;
    uint64_t hot_threshold;
    uint64_t cold_threshold;
    uint64_t last_cleanup;
    uint64_t cleanup_interval;

public:
    FrequencyBasedMigrationPolicy(uint64_t hot = 100, uint64_t cold = 10, uint64_t interval = 10000000)
        : hot_threshold(hot), cold_threshold(cold), last_cleanup(0), cleanup_interval(interval) {}

    void record_access(uint64_t addr) { access_count[addr]++; }

    int compute_once(CXLController *controller) override {
        for (const auto &[timestamp, info] : controller->occupation) {
            record_access(info.address);
        }

        uint64_t current_time = controller->last_timestamp;
        if (current_time - last_cleanup > cleanup_interval) {
            access_count.clear();
            last_cleanup = current_time;
        }

        auto migration_list = get_migration_list(controller);
        return migration_list.empty() ? 0 : 1;
    }

    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> to_migrate;

        int per_size;
        switch (controller->page_type_) {
        case CACHELINE:
            per_size = 64;
            break;
        case PAGE:
            per_size = 4096;
            break;
        case HUGEPAGE_2M:
            per_size = 2 * 1024 * 1024;
            break;
        case HUGEPAGE_1G:
            per_size = 1024 * 1024 * 1024;
            break;
        };

        for (const auto &[timestamp, info] : controller->occupation) {
            uint64_t addr = info.address;
            if (access_count[addr] < cold_threshold) {

                to_migrate.emplace_back(addr, per_size);
            }
        }

        auto collect_hot_data = [this, &to_migrate, per_size](CXLMemExpander *expander) {
            for (const auto &info : expander->occupation) {
                uint64_t addr = info.address;
                if (access_count[addr] > hot_threshold) {

                    to_migrate.emplace_back(addr, per_size);
                }
            }
        };

        for (auto expander : controller->expanders) {
            collect_hot_data(expander);
        }

        std::function<void(CXLSwitch *)> collect_from_switch = [&collect_hot_data,
                                                                &collect_from_switch](CXLSwitch *sw) {
            if (!sw)
                return;

            for (auto expander : sw->expanders) {
                collect_hot_data(expander);
            }

            for (auto child_sw : sw->switches) {
                collect_from_switch(child_sw);
            }
        };

        collect_from_switch(controller);

        return to_migrate;
    }
};

class LoadBalancingMigrationPolicy : public MigrationPolicy {
public:
    double imbalance_threshold;
    uint64_t migration_interval;
    uint64_t last_migration;
    LoadBalancingMigrationPolicy(double threshold = 0.2, uint64_t interval = 5000000)
        : imbalance_threshold(threshold), migration_interval(interval), last_migration(0) {}

    int compute_once(CXLController *controller) override {
        uint64_t current_time = controller->last_timestamp;
        if (current_time - last_migration < migration_interval) {
            return 0;
        }

        struct DeviceLoad {
            CXLMemExpander *expander;
            uint64_t load;
        };

        std::vector<DeviceLoad> expander_loads;

        for (auto expander : controller->expanders) {
            uint64_t load = expander->counter.load + expander->counter.store;
            expander_loads.push_back({expander, load});
        }

        std::function<void(CXLSwitch *)> collect_loads = [&expander_loads, &collect_loads](CXLSwitch *sw) {
            if (!sw)
                return;

            for (auto expander : sw->expanders) {
                uint64_t load = expander->counter.load + expander->counter.store;
                expander_loads.push_back({expander, load});
            }

            for (auto child_sw : sw->switches) {
                collect_loads(child_sw);
            }
        };

        collect_loads(controller);

        if (expander_loads.empty()) {
            return 0;
        }

        auto max_it = std::max_element(expander_loads.begin(), expander_loads.end(),
                                       [](const DeviceLoad &a, const DeviceLoad &b) { return a.load < b.load; });

        auto min_it = std::min_element(expander_loads.begin(), expander_loads.end(),
                                       [](const DeviceLoad &a, const DeviceLoad &b) { return a.load < b.load; });

        uint64_t highest_load = max_it->load;
        uint64_t lowest_load = min_it->load;

        if (highest_load > 0 && static_cast<double>(highest_load - lowest_load) / highest_load > imbalance_threshold) {
            last_migration = current_time;
            return 1;
        }

        return 0;
    }

    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> to_migrate;

        int per_size;
        switch (controller->page_type_) {
        case CACHELINE:
            per_size = 64;
            break;
        case PAGE:
            per_size = 4096;
            break;
        case HUGEPAGE_2M:
            per_size = 2 * 1024 * 1024;
            break;
        case HUGEPAGE_1G:
            per_size = 1024 * 1024 * 1024;
            break;
        };

        struct DeviceLoad {
            CXLMemExpander *expander;
            uint64_t load;
        };

        std::vector<DeviceLoad> expander_loads;

        auto collect_all_loads = [&expander_loads](CXLMemExpander *expander) {
            uint64_t load = expander->counter.load + expander->counter.store;
            expander_loads.push_back({expander, load});
        };

        for (auto expander : controller->expanders) {
            collect_all_loads(expander);
        }

        std::function<void(CXLSwitch *)> collect_loads = [&collect_all_loads, &collect_loads](CXLSwitch *sw) {
            if (!sw)
                return;

            for (auto expander : sw->expanders) {
                collect_all_loads(expander);
            }

            for (auto child_sw : sw->switches) {
                collect_loads(child_sw);
            }
        };

        collect_loads(controller);

        if (expander_loads.empty()) {
            return to_migrate;
        }

        auto max_it = std::max_element(expander_loads.begin(), expander_loads.end(),
                                       [](const DeviceLoad &a, const DeviceLoad &b) { return a.load < b.load; });

        CXLMemExpander *highest_load_expander = max_it->expander;
        int migration_count = 0;
        for (const auto &info : highest_load_expander->occupation) {
            to_migrate.emplace_back(info.address, per_size);
            if (++migration_count >= 5)
                break;
        }

        return to_migrate;
    }
};

class LocalityBasedMigrationPolicy : public MigrationPolicy {
public:
    std::unordered_map<uint64_t, std::vector<uint64_t>> page_access_pattern;
    uint64_t pattern_threshold;
    uint64_t page_size;
    LocalityBasedMigrationPolicy(uint64_t threshold = 5, uint64_t p_size = 4096)
        : pattern_threshold(threshold), page_size(p_size) {}

    void record_access(uint64_t addr) {
        uint64_t page_addr = addr & ~(page_size - 1);
        page_access_pattern[page_addr].push_back(addr);

        if (page_access_pattern[page_addr].size() > 100) {
            page_access_pattern[page_addr].erase(page_access_pattern[page_addr].begin());
        }
    }

    bool has_locality_pattern(uint64_t page_addr) {
        std::unordered_map<uint64_t, int> addr_count;
        for (uint64_t addr : page_access_pattern[page_addr]) {
            addr_count[addr]++;
            if (addr_count[addr] >= pattern_threshold) {
                return true;
            }
        }
        return false;
    }

    int compute_once(CXLController *controller) override {
        for (const auto &[timestamp, info] : controller->occupation) {
            record_access(info.address);
        }

        auto migration_list = get_migration_list(controller);
        return migration_list.empty() ? 0 : 1;
    }

    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> to_migrate;

        for (const auto &[page_addr, accesses] : page_access_pattern) {
            if (has_locality_pattern(page_addr)) {

                bool in_controller = false;
                for (const auto &[timestamp, info] : controller->occupation) {
                    if ((info.address & ~(page_size - 1)) == page_addr) {
                        in_controller = true;
                        break;
                    }
                }

                if (!in_controller) {

                    to_migrate.emplace_back(page_addr, page_size);
                }
            }
        }

        return to_migrate;
    }
};

class LifetimeBasedMigrationPolicy : public MigrationPolicy {
public:
    uint64_t lifetime_threshold;
    LifetimeBasedMigrationPolicy(uint64_t threshold = 1000000) : lifetime_threshold(threshold) {}

    int compute_once(CXLController *controller) override {
        auto migration_list = get_migration_list(controller);
        return migration_list.empty() ? 0 : 1;
    }

    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> to_migrate;

        int per_size;
        switch (controller->page_type_) {
        case CACHELINE:
            per_size = 64;
            break;
        case PAGE:
            per_size = 4096;
            break;
        case HUGEPAGE_2M:
            per_size = 2 * 1024 * 1024;
            break;
        case HUGEPAGE_1G:
            per_size = 1024 * 1024 * 1024;
            break;
        };

        uint64_t current_time = controller->last_timestamp;

        for (const auto &[timestamp, info] : controller->occupation) {
            if (current_time - timestamp > lifetime_threshold) {

                to_migrate.emplace_back(info.address, per_size);
            }
        }

        return to_migrate;
    }
};

class HybridMigrationPolicy : public MigrationPolicy {
public:
    std::vector<MigrationPolicy *> policies;
    HybridMigrationPolicy() {}

    void add_policy(MigrationPolicy *policy) { policies.push_back(policy); }

    int compute_once(CXLController *controller) override {
        int result = 0;

        for (auto policy : policies) {
            result |= policy->compute_once(controller);
        }

        return result;
    }

    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> to_migrate;

        for (auto policy : policies) {
            auto list = policy->get_migration_list(controller);
            to_migrate.insert(to_migrate.end(), list.begin(), list.end());
        }

        std::sort(to_migrate.begin(), to_migrate.end());
        to_migrate.erase(std::unique(to_migrate.begin(), to_migrate.end()), to_migrate.end());

        return to_migrate;
    }
};
#endif // CXLMEMSIM_POLICY_H
