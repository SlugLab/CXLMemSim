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

#include "cxlendpoint.h"
#include "lbr.h"
#include <queue>
#include <string_view>

class Monitors;
struct mem_stats;
struct proc_info;
struct lbr;
struct cntr;
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
};

class MigrationPolicy : public Policy {
public:
    MigrationPolicy() = default;
    virtual ~MigrationPolicy() = default;

    // 基本的compute_once方法，决定是否需要执行迁移
    int compute_once(CXLController *controller) override {
        auto migration_list = get_migration_list(controller);
        return migration_list.empty() ? 0 : 1;
    }

    // 获取需要迁移的地址列表
    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> migration_list;
        // 基类提供空实现
        return migration_list;
    }
    // 判断特定地址是否应该迁移
    virtual bool should_migrate(uint64_t addr, uint64_t timestamp, int current_device) { return false; }

    // 为给定地址选择最佳的目标设备
    virtual int select_target_device(uint64_t addr, int current_device, CXLController *controller) {
        return -1; // -1表示不迁移
    }
};

// need to give a timeout and will be added latency later,
class PagingPolicy : public Policy {
public:
    PagingPolicy();
    int compute_once(CXLController *) override{};
    // paging related
};

class CachingPolicy : public Policy {
public:
    CachingPolicy();

    // 判断是否应该缓存数据
    virtual bool should_cache(uint64_t addr, uint64_t timestamp) {
        return true; // 默认行为，可以被子类覆盖
    }

    // 判断是否应该进行后向失效
    virtual bool should_invalidate(uint64_t addr, uint64_t timestamp) {
        return false; // 默认行为，可以被子类覆盖
    }

    // 获取需要失效的地址列表
    virtual std::vector<uint64_t> get_invalidation_list(CXLController *controller) {
        return {}; // 默认行为，可以被子类覆盖
    }
};

struct LRUCacheEntry {
    uint64_t key; // 缓存键（地址）
    uint64_t value; // 缓存值
    uint64_t timestamp; // 最后访问时间戳
};

// LRU缓存
class LRUCache {
public:
    int capacity; // 缓存容量
    std::unordered_map<uint64_t, LRUCacheEntry> cache; // 缓存映射
    std::list<uint64_t> lru_list; // LRU列表，最近使用的在前面
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> lru_map; // 用于O(1)查找列表位置

    explicit LRUCache(int size) : capacity(size) {}

    // 获取缓存值，如果存在则更新访问顺序
    std::optional<uint64_t> get(uint64_t key, uint64_t timestamp) {
        auto it = cache.find(key);
        if (it == cache.end()) {
            return std::nullopt; // 缓存未命中
        }

        // 更新访问顺序
        lru_list.erase(lru_map[key]);
        lru_list.push_front(key);
        lru_map[key] = lru_list.begin();

        // 更新时间戳
        it->second.timestamp = timestamp;

        return it->second.value; // 返回缓存值
    }

    // 添加或更新缓存
    void put(uint64_t key, uint64_t value, uint64_t timestamp) {
        // 如果已存在，先移除旧位置
        if (cache.find(key) != cache.end()) {
            lru_list.erase(lru_map[key]);
        }
        // 如果缓存已满，移除最久未使用的项
        else if (cache.size() >= capacity) {
            uint64_t lru_key = lru_list.back();
            lru_list.pop_back();
            lru_map.erase(lru_key);
            cache.erase(lru_key);
        }

        // 添加新项到最前面（最近使用）
        lru_list.push_front(key);
        lru_map[key] = lru_list.begin();
        cache[key] = {key, value, timestamp};
    }

    // 获取缓存使用情况统计
    std::tuple<int, int> get_stats() { return {cache.size(), capacity}; }

    // 清除缓存
    void clear() {
        cache.clear();
        lru_list.clear();
        lru_map.clear();
    }
    // LRU缓存类中添加size()方法
    size_t size() const { return cache.size(); }

    // LRU缓存类中添加remove()方法
    bool remove(uint64_t key) {
        auto it = cache.find(key);
        if (it == cache.end()) {
            return false; // 键不存在
        }

        // 从LRU列表中移除
        lru_list.erase(lru_map[key]);

        // 从映射中移除
        lru_map.erase(key);

        // 从缓存中移除
        cache.erase(key);

        return true;
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
    void set_stats(mem_stats stats);
    static void set_process_info(const proc_info &process_info);
    static void set_thread_info(const proc_info &thread_info);
    void perform_migration();
    // 添加缓存访问方法
    std::optional<uint64_t> access_cache(uint64_t addr, uint64_t timestamp) { return lru_cache.get(addr, timestamp); }

    // 添加缓存更新方法
    void update_cache(uint64_t addr, uint64_t value, uint64_t timestamp) { lru_cache.put(addr, value, timestamp); }
    void perform_back_invalidation();
    void invalidate_in_expanders(uint64_t addr);
    void invalidate_in_switch(CXLSwitch *switch_, uint64_t addr);
};

template <> struct std::formatter<CXLController> {
    // Parse function to handle any format specifiers (if needed)
    constexpr auto parse(std::format_parse_context &ctx) -> decltype(ctx.begin()) {
        // If you have specific format specifiers, parse them here
        // For simplicity, we'll ignore them and return the end iterator
        return ctx.end();
    }

    // Format function to output the Monitors data

    template <typename FormatContext>
    auto format(const CXLController &controller, FormatContext &ctx) const -> decltype(ctx.out()) {
        std::string result;

        // 首先打印控制器自身的计数器信息
        result += std::format("CXLController:\n");
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

        // 打印全局计数器
        result += std::format("  Global Counter:\n");
        result += std::format("    Local: {}\n", controller.counter.local.get());
        result += std::format("    Remote: {}\n", controller.counter.remote.get());
        result += std::format("    HITM: {}\n", controller.counter.hitm.get());

        // 打印拓扑结构（交换机和端点）
        result += "Topology:\n";

        // 递归打印每个交换机
        std::function<void(const CXLSwitch *, int)> print_switch = [&result, &print_switch](const CXLSwitch *sw,
                                                                                            int depth) {
            std::string indent(depth * 2, ' ');

            // 打印交换机事件计数
            result += std::format("{}Switch:\n", indent);
            result += std::format("{}  Events:\n", indent);
            result += std::format("{}    Load: {}\n", indent, sw->counter.load.get());
            result += std::format("{}    Store: {}\n", indent, sw->counter.store.get());
            result += std::format("{}    Conflict: {}\n", indent, sw->counter.conflict.get());

            // 递归打印子交换机
            for (const auto &child : sw->switches) {
                print_switch(child, depth + 1);
            }

            // 打印端点
            for (const auto &endpoint : sw->expanders) {
                result += std::format("{}Expander:\n", indent + "  ");
                result += std::format("{}  Events:\n", indent + "  ");
                result += std::format("{}    Load: {}\n", indent + "  ", endpoint->counter.load.get());
                result += std::format("{}    Store: {}\n", indent + "  ", endpoint->counter.store.get());
                result += std::format("{}    Migrate: {}\n", indent + "  ", endpoint->counter.migrate_in.get());
                result += std::format("{}    Migrate: {}\n", indent + "  ", endpoint->counter.migrate_out.get());
                result += std::format("{}    Hit Old: {}\n", indent + "  ", endpoint->counter.hit_old.get());
            }
        };

        // 从控制器开始递归打印
        print_switch(&controller, 0);

        // 打印额外的统计信息
        result += "\nStatistics:\n";
        result += std::format("  Number of Switches: {}\n", controller.num_switches);
        result += std::format("  Number of Endpoints: {}\n", controller.num_end_points);
        result += std::format("  Number of Threads created: {}\n", controller.thread_map.size());
        result += std::format("  Memory Freed: {} bytes\n", controller.freed);

        return format_to(ctx.out(), "{}", result);
    }
};

extern CXLController *controller;
#endif // CXLMEMSIM_CXLCONTROLLER_H
