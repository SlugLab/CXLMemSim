/*
 * CXLMemSim endpoint
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#ifndef CXLMEMSIM_CXLENDPOINT_H
#define CXLMEMSIM_CXLENDPOINT_H

#include "cxlcounter.h"
#include "helper.h"
#include <list>
#include <queue>
#include <map>
#include <tuple>
#include <unordered_set>
#include <vector>
#define ROB_SIZE 512

struct occupation_info {
    uint64_t timestamp{};
    uint64_t address{};
    uint64_t access_count{};
};
struct rob_info {
    std::map<int, int64_t> m_bandwidth, m_count;
    int64_t llcm_base, llcm_count, ins_count;
};
struct thread_info {
    rob_info rob;
    std::queue<int> llcm_type;
    std::queue<int> llcm_type_rob;
};
// Forward declarations
class CXLController;
class CXLEndPoint {
public:
    virtual ~CXLEndPoint() = default;

private:
    virtual void set_epoch(int epoch) = 0;
    virtual void free_stats(double size) = 0;
    virtual void delete_entry(uint64_t addr, uint64_t length) = 0;
    virtual double calculate_latency(const std::vector<std::tuple<uint64_t, uint64_t>> &elem,
                                     double dramlatency) = 0; // traverse the tree to calculate the latency
    virtual double calculate_bandwidth(const std::vector<std::tuple<uint64_t, uint64_t>> &elem) = 0;
    virtual int insert(uint64_t timestamp, uint64_t tid, uint64_t phys_addr, uint64_t virt_addr,
                       int index) = 0; // 0 not this endpoint, 1 store, 2 load, 3 prefetch
    virtual std::vector<std::tuple<uint64_t, uint64_t>> get_access(uint64_t timestamp) = 0;
};

class CXLMemExpander : public CXLEndPoint {
public:
    EmuCXLBandwidth bandwidth{};
    EmuCXLLatency latency{};
    uint64_t capacity;

    std::vector<occupation_info> occupation; // timestamp, pa
    std::unordered_set<uint64_t> address_cache{};
    bool cache_valid = false;
    CXLMemExpanderEvent counter{};
    CXLMemExpanderEvent last_counter{};
    mutable std::shared_mutex occupationMutex_; // 使用共享互斥锁允许多个读取者
    // LRUCache lru_cache;
    // tlb map and paging map -> invalidate
    int last_read = 0;
    int last_write = 0;
    double last_latency = 0.;
    int epoch = 0;
    uint64_t last_timestamp = 0;
    int id = -1;
    struct AddressRange {
        uint64_t start;
        uint64_t end;

        // 添加比较运算符，用于二分查找
        bool operator<(const AddressRange& other) const {
            return end < other.start;
        }

        bool operator<(uint64_t addr) const {
            return end < addr;
        }
    };
    std::vector<AddressRange> address_ranges;

    CXLMemExpander(int read_bw, int write_bw, int read_lat, int write_lat, int id, int capacity);
    std::vector<std::tuple<uint64_t, uint64_t>> get_access(uint64_t timestamp) override;
    void set_epoch(int epoch) override;
    void free_stats(double size) override;
    int insert(uint64_t timestamp, uint64_t tid, uint64_t phys_addr, uint64_t virt_addr, int index) override;
    double calculate_latency(const std::vector<std::tuple<uint64_t, uint64_t>> &elem,
                             double dramlatency) override; // traverse the tree to calculate the latency
    double calculate_bandwidth(const std::vector<std::tuple<uint64_t, uint64_t>> &elem) override;
    void delete_entry(uint64_t addr, uint64_t length) override;
    void update_address_cache() {
        if (cache_valid) return;
        address_cache.clear();
        for (const auto& occ : occupation)
            address_cache.insert(occ.address);
        cache_valid = true;
    }
    // 当 occupation 更新时调用此函数
    void invalidate_cache() {
        cache_valid = false;
    }

    void update_range_cache() {
        if (cache_valid) return;
        address_ranges.clear();
        // 排序occupation以便合并连续地址
        std::sort(occupation.begin(), occupation.end(),
                 [](const auto& a, const auto& b) { return a.address < b.address; });
        if (occupation.empty()) {
            cache_valid = true;
            return;
        }
        AddressRange current{occupation[0].address, occupation[0].address};
        for (size_t i = 1; i < occupation.size(); i++) {
            if (occupation[i].address == current.end + 1) {
                // 连续地址，扩展范围
                current.end = occupation[i].address;
            } else {
                // 新范围
                address_ranges.push_back(current);
                current = {occupation[i].address, occupation[i].address};
            }
        }
        address_ranges.push_back(current);
        cache_valid = true;
    }

    bool is_address_local(uint64_t addr) {
        if (!cache_valid)
            update_range_cache();
        // 使用标准库的二分查找，查找第一个"不小于"addr的元素
        auto it = std::lower_bound(address_ranges.begin(), address_ranges.end(), addr);
        // 如果找到了范围，且addr在这个范围内
        if (it != address_ranges.end() && addr >= it->start && addr <= it->end)
            return true;
        return false;
    }
};
class CXLSwitch : public CXLEndPoint {
public:
    std::vector<CXLMemExpander *> expanders{};
    std::vector<CXLSwitch *> switches{};
    CXLSwitchEvent counter{};
    int id = -1;
    int epoch = 0;
    uint64_t last_timestamp = 0;
    // TODO get the approximate congestion and target done time
    std::unordered_map<uint64_t, uint64_t> timeseries_map;

    double congestion_latency = 0.02; // 200ns is the latency of the switch
    explicit CXLSwitch(int id);
    std::vector<std::tuple<uint64_t, uint64_t>> get_access(uint64_t timestamp) override;
    double calculate_latency(const std::vector<std::tuple<uint64_t, uint64_t>> &elem,
                             double dramlatency) override; // traverse the tree to calculate the latency
    double calculate_bandwidth(const std::vector<std::tuple<uint64_t, uint64_t>> &elem) override;
    double get_endpoint_rob_latency(CXLMemExpander* endpoint,
                                  const std::vector<std::tuple<uint64_t, uint64_t>>& accesses,
                                  const thread_info& t_info,
                                  double dramlatency);
    int insert(uint64_t timestamp, uint64_t tid, uint64_t phys_addr, uint64_t virt_addr, int index) override;
    void delete_entry(uint64_t addr, uint64_t length) override;
    virtual std::tuple<double, std::vector<uint64_t>> calculate_congestion();
    void set_epoch(int epoch) override;
    void free_stats(double size) override;
};

#endif // CXLMEMSIM_CXLENDPOINT_H
