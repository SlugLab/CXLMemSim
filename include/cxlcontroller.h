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
#include <shared_mutex>
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
    virtual ~AllocationPolicy() = default;
    int compute_once(CXLController *controller) override { return 0; };
};

class MigrationPolicy : public Policy {
public:
    MigrationPolicy() = default;
    virtual ~MigrationPolicy() = default;

    // 基本的compute_once方法，决定是否需要执行迁移
    // Basic compute_once method, determines whether migration should be executed
    int compute_once(CXLController *controller) override {
        auto migration_list = get_migration_list(controller);
        return migration_list.empty() ? 0 : 1;
    }

    // 获取需要迁移的地址列表
    // Get the list of addresses that need migration
    std::vector<std::tuple<uint64_t, uint64_t>> get_migration_list(CXLController *controller) {
        std::vector<std::tuple<uint64_t, uint64_t>> migration_list;
        // 基类提供空实现
        // Base class provides empty implementation
        return migration_list;
    }
    // 判断特定地址是否应该迁移
    // Determine if a specific address should be migrated
    virtual bool should_migrate(uint64_t addr, uint64_t timestamp, int current_device) { return false; }

    // 为给定地址选择最佳的目标设备
    // Select the best target device for a given address
    virtual int select_target_device(uint64_t addr, int current_device, CXLController *controller) {
        return -1; // -1表示不迁移
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

    // 判断是否应该缓存数据
    // Determine if data should be cached
    virtual bool should_cache(uint64_t addr, uint64_t timestamp) {
        return true; // 默认行为，可以被子类覆盖
                     // Default behavior, can be overridden by subclasses
    }

    // 判断是否应该进行后向失效
    // Determine if backward invalidation should be performed
    virtual bool should_invalidate(uint64_t addr, uint64_t timestamp) {
        return false; // 默认行为，可以被子类覆盖
                      // Default behavior, can be overridden by subclasses
    }

    // 获取需要失效的地址列表
    // Get the list of addresses that need invalidation
    virtual std::vector<uint64_t> get_invalidation_list(CXLController *controller) {
        return {}; // 默认行为，可以被子类覆盖
                   // Default behavior, can be overridden by subclasses
    }
};

struct LRUCacheEntry {
    uint64_t key; // Cache key (address)
    uint64_t value; // Cache value
    uint64_t timestamp; // Last access timestamp
};
// 使用读写锁的线程安全LRU缓存
class LRUCache {
public:
    int capacity; // 缓存容量
    std::unordered_map<uint64_t, LRUCacheEntry> cache; // 缓存映射
    std::list<uint64_t> lru_list; // LRU列表，最近使用的在前面
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> lru_map; // 用于O(1)查找列表位置
    mutable std::shared_mutex rwmutex_; // 读写锁
    explicit LRUCache(int size) : capacity(size) {}
    // 获取缓存值，如果存在则更新访问顺序（需要写锁，因为会修改LRU顺序）
    std::optional<uint64_t> get(uint64_t key, uint64_t timestamp) {
        // 首先尝试以共享锁方式检查键是否存在
        {
            std::shared_lock<std::shared_mutex> readLock(rwmutex_);
            auto it = cache.find(key);
            if (it == cache.end()) {
                return std::nullopt; // 缓存未命中
            }
        }
        // 如果键存在，需要升级到独占锁进行更新
        std::unique_lock<std::shared_mutex> writeLock(rwmutex_);
        auto it = cache.find(key);
        if (it == cache.end()) {
            return std::nullopt; // 在获取写锁期间键可能已被删除
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
        std::unique_lock<std::shared_mutex> writeLock(rwmutex_);
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
    // 获取缓存使用情况统计（只读操作）
    std::tuple<int, int> get_stats() const {
        std::shared_lock<std::shared_mutex> readLock(rwmutex_);
        return {cache.size(), capacity};
    }
    // 清除缓存
    void clear() {
        std::unique_lock<std::shared_mutex> writeLock(rwmutex_);
        cache.clear();
        lru_list.clear();
        lru_map.clear();
    }
    // 获取缓存大小（只读操作）
    size_t size() const {
        std::shared_lock<std::shared_mutex> readLock(rwmutex_);
        return cache.size();
    }
    // 移除指定键
    bool remove(uint64_t key) {
        std::unique_lock<std::shared_mutex> writeLock(rwmutex_);
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
    // 判断键是否存在（只读操作，不更新LRU顺序）
    bool contains(uint64_t key) const {
        std::shared_lock<std::shared_mutex> readLock(rwmutex_);
        return cache.find(key) != cache.end();
    }

    // 添加一个查看值但不更新LRU顺序的方法（只读操作）
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
#ifndef SERVER_MODE
    void insert_one(thread_info &t_info, lbr &lbr);
    int insert(uint64_t timestamp, uint64_t tid, lbr lbrs[32], cntr counters[32]);
#endif
    int insert(uint64_t timestamp, uint64_t tid, uint64_t phys_addr, uint64_t virt_addr, int index) override;
    void delete_entry(uint64_t addr, uint64_t length) override;
#ifndef SERVER_MODE
    void set_stats(mem_stats stats);
    void set_process_info(const proc_info &process_info);
    void set_thread_info(const proc_info &thread_info);
#endif
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
                result += std::format("{}    Migrate in: {}\n", indent + "  ", endpoint->counter.migrate_in.get());
                result += std::format("{}    Migrate out: {}\n", indent + "  ", endpoint->counter.migrate_out.get());
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
