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
    HugePagePolicy() = default;
    int compute_once(CXLController *) override;
};

class FIFOPolicy : public CachingPolicy {
public:
    FIFOPolicy() = default;
    int compute_once(CXLController *) override;
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
private:
    double imbalance_threshold; // 负载不平衡阈值
    uint64_t migration_interval; // 迁移间隔
    uint64_t last_migration; // 上次迁移时间

public:
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
private:
    std::unordered_map<uint64_t, std::vector<uint64_t>> page_access_pattern; // 页面访问模式
    uint64_t pattern_threshold; // 模式识别阈值
    uint64_t page_size; // 页面大小

public:
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
private:
    uint64_t lifetime_threshold; // 数据寿命阈值

public:
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
        case CACHELINE: per_size = 64; break;
        case PAGE: per_size = 4096; break;
        case HUGEPAGE_2M: per_size = 2 * 1024 * 1024; break;
        case HUGEPAGE_1G: per_size = 1024 * 1024 * 1024; break;
        };

        uint64_t current_time = controller->last_timestamp;

        // 从控制器中查找生命周期较长的数据
        for (const auto& [timestamp, info] : controller->occupation) {
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
private:
    std::vector<MigrationPolicy*> policies;  // 多个迁移策略

public:
    HybridMigrationPolicy() {}

    // 添加策略
    void add_policy(MigrationPolicy* policy) {
        policies.push_back(policy);
    }

    int compute_once(CXLController* controller) override {
        int result = 0;

        // 运行所有策略
        for (auto policy : policies) {
            result |= policy->compute_once(controller);
        }

        return result;
    }

    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController* controller) {
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
