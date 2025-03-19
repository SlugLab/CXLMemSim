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
    std::vector<double> latency_scores; // 存储每个节点的延迟评分
    int compute_once(CXLController *) override;
};

class HeatAwareMigrationPolicy : public MigrationPolicy {
public:
    std::unordered_map<uint64_t, uint64_t> access_count; // 地址到访问次数的映射
    uint64_t hot_threshold; // 热点数据阈值

    HeatAwareMigrationPolicy(uint64_t threshold = 100) : hot_threshold(threshold) {}

    // 记录访问以跟踪热点数据
    void record_access(uint64_t addr) { access_count[addr]++; }

    int compute_once(CXLController *controller) override {
        // 更新访问计数
        for (const auto &[timestamp, info] : controller->occupation) {
            record_access(info.address);
        }

        // 检查是否有热点数据需要迁移
        auto migration_list = get_migration_list(controller);
        return migration_list.empty() ? 0 : 1;
    }

    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> to_migrate;

        // 定义页面大小
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

        // 检查热点数据
        for (const auto &[addr, count] : access_count) {
            if (count > hot_threshold) {
                // 添加到迁移列表
                to_migrate.emplace_back(addr, per_size);
            }
        }
        return to_migrate;
    }
};

class HugePagePolicy : public PagingPolicy {
public:
    // 页表遍历延迟基准值（纳秒）
    uint64_t ptw_base_latency_local; // 本地内存页表遍历基准延迟
    uint64_t ptw_base_latency_remote; // 远程内存页表遍历基准延迟

    // 页表缓存，用于追踪已经转换过的虚拟地址
    // 键：虚拟地址，值：物理地址
    std::unordered_map<uint64_t, uint64_t> va_pa_cache;

    // TLB模拟（按页面大小分类）
    struct TLBCache {
        std::list<uint64_t> entries; // LRU顺序的页面地址
        std::unordered_map<uint64_t, std::list<uint64_t>::iterator> index; // 快速查找
        size_t capacity; // TLB大小

        TLBCache(size_t size) : capacity(size) {}

        bool lookup(uint64_t page_addr) {
            auto it = index.find(page_addr);
            if (it != index.end()) {
                // 命中，将条目移到最前面
                entries.erase(it->second);
                entries.push_front(page_addr);
                it->second = entries.begin();
                return true;
            }
            return false;
        }

        void insert(uint64_t page_addr) {
            // 如果已存在，先移除
            auto it = index.find(page_addr);
            if (it != index.end()) {
                entries.erase(it->second);
                index.erase(it);
            }

            // 如果已满，移除最旧的条目
            if (entries.size() >= capacity) {
                uint64_t victim = entries.back();
                entries.pop_back();
                index.erase(victim);
            }

            // 插入新条目
            entries.push_front(page_addr);
            index[page_addr] = entries.begin();
        }
    };

    // 不同大小的TLB
    TLBCache tlb_4k; // 4KB页面的TLB
    TLBCache tlb_2m; // 2MB页面的TLB
    TLBCache tlb_1g; // 1GB页面的TLB
    CXLHugePageEvent stats; // 统计信息
    explicit HugePagePolicy(uint64_t local_latency = 100, uint64_t remote_latency = 300)
        : ptw_base_latency_local(local_latency), ptw_base_latency_remote(remote_latency),
          tlb_4k(64), // 4KB页面TLB容量(较大)
          tlb_2m(32), // 2MB页面TLB容量(中等)
          tlb_1g(4) // 1GB页面TLB容量(较小)
    {}

    // 检查页表遍历延迟
    uint64_t check_page_table_walk(uint64_t virt_addr, uint64_t phys_addr, bool is_remote,
                                   page_type page_size) override {
        // 基础延迟
        uint64_t base_latency = is_remote ? ptw_base_latency_remote : ptw_base_latency_local;

        // 根据页面大小获取页面地址和对应的TLB
        uint64_t page_addr;
        TLBCache *tlb;
        double ptw_reduction = 1.0; // 页表遍历延迟减少因子

        switch (page_size) {
        case CACHELINE:
        case PAGE:
            // 4KB页面
            page_addr = virt_addr & ~((1ULL << 12) - 1);
            tlb = &tlb_4k;
            ptw_reduction = 1.0; // 标准4级页表遍历
            break;

        case HUGEPAGE_2M:
            // 2MB页面
            page_addr = virt_addr & ~((1ULL << 21) - 1);
            tlb = &tlb_2m;
            ptw_reduction = 0.75; // 减少一级页表遍历，大约节省25%延迟
            break;

        case HUGEPAGE_1G:
            // 1GB页面
            page_addr = virt_addr & ~((1ULL << 30) - 1);
            tlb = &tlb_1g;
            ptw_reduction = 0.5; // 减少两级页表遍历，大约节省50%延迟
            break;

        default:
            // 默认按4KB页面处理
            page_addr = virt_addr & ~((1ULL << 12) - 1);
            tlb = &tlb_4k;
            ptw_reduction = 1.0;
        }

        // 检查TLB命中
        bool tlb_hit = tlb->lookup(page_addr);

        // 更新统计信息
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
            // TLB命中，无需页表遍历
            return 0;
        }

        // TLB缺失，需要页表遍历
        // 检查VA-PA缓存
        auto it = va_pa_cache.find(virt_addr);
        if (it != va_pa_cache.end()) {
            // 虚拟地址已缓存，但TLB缺失，只需要更新TLB
            tlb->insert(page_addr);
            // 返回较小的延迟，表示只需从页表缓存加载
            return base_latency * 0.2 * ptw_reduction;
        }

        // 完整的页表遍历
        stats.inc_ptw_count();

        // 更新缓存
        va_pa_cache[virt_addr] = phys_addr;
        tlb->insert(page_addr);

        // 返回完整页表遍历延迟，根据页面大小调整
        return base_latency * ptw_reduction;
    }

    int compute_once(CXLController *controller) override {
        // 分析内存访问模式以决定是否应该合并为大页
        std::unordered_map<uint64_t, std::vector<uint64_t>> page_groups;
        size_t potential_huge_pages = 0;

        // 分析当前的内存访问，按照潜在的大页边界进行分组
        for (const auto &[timestamp, info] : controller->occupation) {
            uint64_t addr = info.address;

            // 计算2MB大页边界（地址的低21位设为0）
            uint64_t huge_page_2m_addr = addr & ~((1ULL << 21) - 1);

            // 将地址添加到对应的2MB页组
            page_groups[huge_page_2m_addr].push_back(addr);
        }

        // 检查哪些页组满足合并为大页的条件
        for (const auto &[huge_page_addr, addresses] : page_groups) {
            // 计算此组中有多少4KB页面
            std::unordered_set<uint64_t> unique_pages;
            for (uint64_t addr : addresses) {
                // 获取4KB页面地址（清除低12位）
                uint64_t page_addr = addr & ~((1ULL << 12) - 1);
                unique_pages.insert(page_addr);
            }

            // 如果此组中的4KB页面数量超过阈值，考虑合并为大页
            // 2MB大页包含512个4KB页面
            if (unique_pages.size() > 64) { // 使用较低阈值，例如1/8的2MB大页
                potential_huge_pages++;
            }
        }

        // 如果潜在的大页数量足够多，建议切换到大页模式
        if (potential_huge_pages > 3) {
            // 切换到2MB大页模式
            controller->page_type_ = HUGEPAGE_2M;
            return 1; // 成功切换到大页模式
        }

        // 检查是否有更大的内存连续区域，可以使用1GB大页
        std::unordered_map<uint64_t, size_t> gb_page_usage;

        for (const auto &[huge_page_addr, addresses] : page_groups) {
            // 计算1GB大页地址（清除低30位）
            uint64_t gb_page_addr = huge_page_addr & ~((1ULL << 30) - 1);
            gb_page_usage[gb_page_addr] += addresses.size();
        }

        // 检查是否有任何1GB页面使用率足够高
        for (const auto &[gb_addr, usage] : gb_page_usage) {
            // 一个1GB页面包含262144个4KB页面
            if (usage > 16384) { // 使用较低阈值，例如1/16的1GB大页
                // 切换到1GB大页模式
                controller->page_type_ = HUGEPAGE_1G;
                return 2; // 成功切换到1GB大页模式
            }
        }

        return 0; // 保持当前页面大小
    }

    // 获取TLB和页表遍历统计信息
    std::tuple<double, double, double, uint64_t> get_stats() const {
        // 计算各种TLB的命中率
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
    // 页表缓存，用于追踪已经转换过的虚拟地址
    std::unordered_map<uint64_t, uint64_t> va_pa_cache;
    // 页表访问延迟（纳秒）
    uint64_t ptw_latency_local; // 本地内存页表遍历延迟
    uint64_t ptw_latency_remote; // 远程内存页表遍历延迟
    // 缓存命中率统计
    CXLPageTableEvent cache_stats;
    // 最近检查时间戳（用于周期性清理）
    uint64_t last_cleanup_timestamp;
    // 清理间隔
    uint64_t cleanup_interval;

    explicit PageTableAwarePolicy(uint64_t local_latency = 100, uint64_t remote_latency = 300,
                                  uint64_t cleanup_interval = 10000000)
        : ptw_latency_local(local_latency), ptw_latency_remote(remote_latency), last_cleanup_timestamp(0),
          cleanup_interval(cleanup_interval) {}

    int compute_once(CXLController *controller) override {
        uint64_t current_timestamp = controller->last_timestamp;

        // 周期性清理缓存
        if (current_timestamp - last_cleanup_timestamp > cleanup_interval) {
            va_pa_cache.clear();
            last_cleanup_timestamp = current_timestamp;
            return 1; // 表示执行了清理操作
        }

        // 计算缓存命中率
        double hit_rate =
            cache_stats.get_total() > 0 ? static_cast<double>(cache_stats.get_hit()) / cache_stats.get_total() : 0.0;

        // 根据命中率动态调整页表缓存大小
        // 如果命中率低，可能需要增加缓存大小
        if (hit_rate < 0.7 && va_pa_cache.size() < 10000) {
            // 保持当前缓存大小，等待更多数据
        } else if (hit_rate > 0.9 && va_pa_cache.size() > 1000) {
            // 可以考虑减小缓存大小，这里简单实现，实际可能需要更复杂的策略
            size_t to_remove = va_pa_cache.size() / 10; // 移除10%

            // 随机选择条目删除
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

            return 2; // 表示执行了缓存调整
        }

        return 0; // 表示没有执行特殊操作
    }

    // 检查是否需要页表遍历，并估计延迟
    uint64_t check_page_table_walk(uint64_t virt_addr, uint64_t phys_addr, bool is_remote,
                                   page_type page_size) override {
        cache_stats.inc_total();

        // 检查缓存中是否已有映射
        if (va_pa_cache.find(virt_addr) != va_pa_cache.end()) {
            cache_stats.inc_hit();
            return 0; // 缓存命中，不需要页表遍历
        }

        // 缓存未命中，需要页表遍历
        cache_stats.inc_miss();

        // 添加到缓存
        va_pa_cache[virt_addr] = phys_addr;

        // 返回估计的页表遍历延迟
        return is_remote ? ptw_latency_remote : ptw_latency_local;
    }

    // 获取页表遍历和TLB管理统计
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
    bool should_cache(uint64_t addr, uint64_t timestamp) override {
        return false;
    };
    bool should_invalidate(uint64_t addr, uint64_t timestamp) override {
        return false;
    };
};

// 基于访问频率的后向失效策略
class FrequencyBasedInvalidationPolicy : public CachingPolicy {
public:
    std::unordered_map<uint64_t, uint64_t> access_count; // 地址到访问计数的映射
    uint64_t access_threshold; // 访问阈值
    uint64_t last_cleanup; // 上次清理时间戳
    uint64_t cleanup_interval; // 清理间隔

    explicit FrequencyBasedInvalidationPolicy(uint64_t threshold = 100, uint64_t interval = 10000000)
        : access_threshold(threshold), last_cleanup(0), cleanup_interval(interval) {}

    bool should_cache(uint64_t addr, uint64_t timestamp);
    bool should_invalidate(uint64_t addr, uint64_t timestamp);

    std::vector<uint64_t> get_invalidation_list(CXLController *controller);

    int compute_once(CXLController *controller) override;
};
// 基于访问频率的迁移策略
class FrequencyBasedMigrationPolicy : public MigrationPolicy {
private:
    std::unordered_map<uint64_t, uint64_t> access_count; // 地址到访问次数的映射
    uint64_t hot_threshold; // 热点数据阈值
    uint64_t cold_threshold; // 冷数据阈值
    uint64_t last_cleanup; // 上次清理时间戳
    uint64_t cleanup_interval; // 清理间隔

public:
    FrequencyBasedMigrationPolicy(uint64_t hot = 100, uint64_t cold = 10, uint64_t interval = 10000000)
        : hot_threshold(hot), cold_threshold(cold), last_cleanup(0), cleanup_interval(interval) {}

    // 记录访问以跟踪访问频率
    void record_access(uint64_t addr) { access_count[addr]++; }

    int compute_once(CXLController *controller) override {
        // 更新访问计数
        for (const auto &[timestamp, info] : controller->occupation) {
            record_access(info.address);
        }

        // 检查是否应该清理访问计数
        uint64_t current_time = controller->last_timestamp;
        if (current_time - last_cleanup > cleanup_interval) {
            access_count.clear();
            last_cleanup = current_time;
        }

        // 检查是否有数据需要迁移
        auto migration_list = get_migration_list(controller);
        return migration_list.empty() ? 0 : 1;
    }

    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> to_migrate;

        // 定义页面大小
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

        // 从控制器中查找冷数据
        for (const auto &[timestamp, info] : controller->occupation) {
            uint64_t addr = info.address;
            if (access_count[addr] < cold_threshold) {
                // 冷数据，可以考虑迁移到远程内存
                to_migrate.emplace_back(addr, per_size);
            }
        }

        // 从扩展器中查找热数据
        auto collect_hot_data = [this, &to_migrate, per_size](CXLMemExpander *expander) {
            for (const auto &info : expander->occupation) {
                uint64_t addr = info.address;
                if (access_count[addr] > hot_threshold) {
                    // 热数据，可以考虑迁移到本地
                    to_migrate.emplace_back(addr, per_size);
                }
            }
        };

        // 收集直接连接的扩展器中的热数据
        for (auto expander : controller->expanders) {
            collect_hot_data(expander);
        }

        // 递归收集子交换机下的扩展器中的热数据
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

// 基于负载平衡的迁移策略
class LoadBalancingMigrationPolicy : public MigrationPolicy {
public:
    double imbalance_threshold; // 负载不平衡阈值
    uint64_t migration_interval; // 迁移间隔
    uint64_t last_migration; // 上次迁移时间

    LoadBalancingMigrationPolicy(double threshold = 0.2, uint64_t interval = 5000000)
        : imbalance_threshold(threshold), migration_interval(interval), last_migration(0) {}

    int compute_once(CXLController *controller) override {
        // 检查是否到达迁移间隔
        uint64_t current_time = controller->last_timestamp;
        if (current_time - last_migration < migration_interval) {
            return 0; // 还未到达迁移时间
        }

        // 计算每个设备的负载
        uint64_t controller_load = controller->counter.local + controller->counter.remote;

        // 计算所有扩展器的负载
        struct DeviceLoad {
            CXLMemExpander *expander;
            uint64_t load;
        };

        std::vector<DeviceLoad> expander_loads;

        // 收集直接连接的扩展器的负载
        for (auto expander : controller->expanders) {
            uint64_t load = expander->counter.load + expander->counter.store;
            expander_loads.push_back({expander, load});
        }

        // 递归收集所有子交换机下的扩展器的负载
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
            return 0; // 没有扩展器
        }

        // 找出负载最高和最低的设备
        auto max_it = std::max_element(expander_loads.begin(), expander_loads.end(),
                                       [](const DeviceLoad &a, const DeviceLoad &b) { return a.load < b.load; });

        auto min_it = std::min_element(expander_loads.begin(), expander_loads.end(),
                                       [](const DeviceLoad &a, const DeviceLoad &b) { return a.load < b.load; });

        CXLMemExpander *highest_load_expander = max_it->expander;
        CXLMemExpander *lowest_load_expander = min_it->expander;
        uint64_t highest_load = max_it->load;
        uint64_t lowest_load = min_it->load;

        // 检查是否有显著的负载不平衡
        if (highest_load > 0 && static_cast<double>(highest_load - lowest_load) / highest_load > imbalance_threshold) {
            // 执行负载平衡
            last_migration = current_time;
            return 1;
        }

        return 0;
    }

    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> to_migrate;

        // 定义页面大小
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

        // 计算所有扩展器的负载
        struct DeviceLoad {
            CXLMemExpander *expander;
            uint64_t load;
        };

        std::vector<DeviceLoad> expander_loads;

        // 收集所有扩展器的负载
        auto collect_all_loads = [&expander_loads](CXLMemExpander *expander) {
            uint64_t load = expander->counter.load + expander->counter.store;
            expander_loads.push_back({expander, load});
        };

        // 直接连接的扩展器
        for (auto expander : controller->expanders) {
            collect_all_loads(expander);
        }

        // 递归收集子交换机的扩展器
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
            return to_migrate; // 没有扩展器
        }

        // 找出负载最高和最低的设备
        auto max_it = std::max_element(expander_loads.begin(), expander_loads.end(),
                                       [](const DeviceLoad &a, const DeviceLoad &b) { return a.load < b.load; });

        auto min_it = std::min_element(expander_loads.begin(), expander_loads.end(),
                                       [](const DeviceLoad &a, const DeviceLoad &b) { return a.load < b.load; });

        CXLMemExpander *highest_load_expander = max_it->expander;
        CXLMemExpander *lowest_load_expander = min_it->expander;

        // 从负载最高的设备选取一些数据迁移到负载最低的设备
        int migration_count = 0;
        for (const auto &info : highest_load_expander->occupation) {
            to_migrate.emplace_back(info.address, per_size);
            if (++migration_count >= 5)
                break; // 限制每次迁移的数量
        }

        return to_migrate;
    }
};

// 基于局部性的迁移策略
class LocalityBasedMigrationPolicy : public MigrationPolicy {
public:
    std::unordered_map<uint64_t, std::vector<uint64_t>> page_access_pattern; // 页面访问模式
    uint64_t pattern_threshold; // 模式识别阈值
    uint64_t page_size; // 页面大小

    LocalityBasedMigrationPolicy(uint64_t threshold = 5, uint64_t p_size = 4096)
        : pattern_threshold(threshold), page_size(p_size) {}

    // 记录访问模式
    void record_access(uint64_t addr) {
        uint64_t page_addr = addr & ~(page_size - 1); // 获取页面地址
        page_access_pattern[page_addr].push_back(addr);

        // 保持访问历史在合理大小
        if (page_access_pattern[page_addr].size() > 100) {
            page_access_pattern[page_addr].erase(page_access_pattern[page_addr].begin());
        }
    }

    // 检查是否有局部性模式
    bool has_locality_pattern(uint64_t page_addr) {
        // 简化的模式检测：检查重复访问
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
        // 更新访问模式
        for (const auto &[timestamp, info] : controller->occupation) {
            record_access(info.address);
        }

        // 检查是否有需要迁移的数据
        auto migration_list = get_migration_list(controller);
        return migration_list.empty() ? 0 : 1;
    }

    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> to_migrate;

        // 遍历所有页面的访问模式
        for (const auto &[page_addr, accesses] : page_access_pattern) {
            if (has_locality_pattern(page_addr)) {
                // 有局部性模式，考虑迁移

                // 查找此页面当前所在的设备
                bool in_controller = false;
                for (const auto &[timestamp, info] : controller->occupation) {
                    if ((info.address & ~(page_size - 1)) == page_addr) {
                        in_controller = true;
                        break;
                    }
                }

                if (!in_controller) {
                    // 页面不在控制器中，可以考虑迁移到控制器
                    to_migrate.emplace_back(page_addr, page_size);
                }
            }
        }

        return to_migrate;
    }
};

// 基于数据寿命的迁移策略
class LifetimeBasedMigrationPolicy : public MigrationPolicy {
public:
    uint64_t lifetime_threshold; // 数据寿命阈值

    LifetimeBasedMigrationPolicy(uint64_t threshold = 1000000) : lifetime_threshold(threshold) {}

    int compute_once(CXLController *controller) override {
        // 获取迁移列表
        auto migration_list = get_migration_list(controller);
        return migration_list.empty() ? 0 : 1;
    }

    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> to_migrate;

        // 定义页面大小
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

        // 从控制器中查找生命周期较长的数据
        for (const auto &[timestamp, info] : controller->occupation) {
            if (current_time - timestamp > lifetime_threshold) {
                // 生命周期较长的数据，可以考虑迁移到远程内存
                to_migrate.emplace_back(info.address, per_size);
            }
        }

        return to_migrate;
    }
};

// 混合多策略迁移
class HybridMigrationPolicy : public MigrationPolicy {
public:
    std::vector<MigrationPolicy *> policies; // 多个迁移策略

    HybridMigrationPolicy() {}

    // 添加策略
    void add_policy(MigrationPolicy *policy) { policies.push_back(policy); }

    int compute_once(CXLController *controller) override {
        int result = 0;

        // 运行所有策略
        for (auto policy : policies) {
            result |= policy->compute_once(controller);
        }

        return result;
    }

    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> to_migrate;

        // 收集所有策略的迁移列表
        for (auto policy : policies) {
            auto list = policy->get_migration_list(controller);
            to_migrate.insert(to_migrate.end(), list.begin(), list.end());
        }

        // 去重
        std::sort(to_migrate.begin(), to_migrate.end());
        to_migrate.erase(std::unique(to_migrate.begin(), to_migrate.end()), to_migrate.end());

        return to_migrate;
    }
};
#endif // CXLMEMSIM_POLICY_H
