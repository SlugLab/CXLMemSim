//
// Created by victoryang00 on 1/13/23.
//

#ifndef CXLMEMSIM_CXLENDPOINT_H
#define CXLMEMSIM_CXLENDPOINT_H
#include "cxlcounter.h"
#include "helper.h"

class LRUCache {
    std::list<uint64_t> lru_list;
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> lru_map;
    std::unordered_map<uint64_t, uint64_t> wb_map;
    size_t capacity;

public:
    LRUCache(size_t cap) : capacity(cap) {}

    void insert(uint64_t key, uint64_t value) {
        // Check if the item is already in the cache
        if (lru_map.find(key) != lru_map.end()) {
            // Move the element to the front of the list
            lru_list.erase(lru_map[key]);
            lru_list.push_front(key);
            lru_map[key] = lru_list.begin();
            wb_map[key] = value;
        } else {
            // If the cache is full, remove the least recently used item
            if (lru_list.size() == capacity) {
                uint64_t old_key = lru_list.back();
                lru_list.pop_back();
                lru_map.erase(old_key);
                wb_map.erase(old_key);
            }
            // Insert the new item
            lru_list.push_front(key);
            lru_map[key] = lru_list.begin();
            wb_map[key] = value;
        }
    }

    uint64_t get(uint64_t key) {
        if (lru_map.find(key) == lru_map.end()) {
            throw std::runtime_error("Key not found");
        }
        // Move the accessed item to the front of the list
        lru_list.erase(lru_map[key]);
        lru_list.push_front(key);
        lru_map[key] = lru_list.begin();
        return wb_map[key];
    }
};

class CXLEndPoint {
    virtual void set_epoch(int epoch) = 0;
    virtual std::string output() = 0;
    virtual void delete_entry(uint64_t addr, uint64_t length) = 0;
    virtual double calculate_latency(LatencyPass elem) = 0; // traverse the tree to calculate the latency
    virtual double calculate_bandwidth(BandwidthPass elem) = 0;
    virtual int insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr,
                       int index) = 0; // 0 not this endpoint, 1 store, 2 load, 3 prefetch
    virtual std::tuple<int, int> get_all_access() = 0;
};

class CXLMemExpander : public CXLEndPoint {
public:
    EmuCXLBandwidth bandwidth;
    EmuCXLLatency latency;
    uint64_t capacity;
    std::map<uint64_t, uint64_t> occupation; // timestamp, pa
    std::map<uint64_t, uint64_t> va_pa_map; // va, pa
    CXLMemExpanderEvent counter{};
    CXLMemExpanderEvent last_counter{};

    LRUCache lru_cache;
    // tlb map and paging map -> invalidate
    int last_read = 0;
    int last_write = 0;
    double last_latency = 0.;
    int epoch = 0;
    uint64_t last_timestamp = 0;
    int id = -1;
    CXLMemExpander(int read_bw, int write_bw, int read_lat, int write_lat, int id, int capacity);
    std::tuple<int, int> get_all_access() override;
    void set_epoch(int epoch) override;
    int insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr, int index) override;
    double calculate_latency(LatencyPass elem) override; // traverse the tree to calculate the latency
    double calculate_bandwidth(BandwidthPass elem) override;
    void delete_entry(uint64_t addr, uint64_t length) override;
    std::string output() override;
};
class CXLSwitch : public CXLEndPoint {
public:
    std::vector<CXLMemExpander *> expanders{};
    std::vector<CXLSwitch *> switches{};
    CXLSwitchEvent counter{};
    int id = -1;
    int epoch = 0;
    uint64_t last_timestamp = 0;
    // get the approximate congestion and target done time
    std::unordered_map<uint64_t, uint64_t> timeseries_map;

    double congestion_latency = 0.02;
    explicit CXLSwitch(int id);
    std::tuple<int, int> get_all_access() override;
    double calculate_latency(LatencyPass elem) override; // traverse the tree to calculate the latency
    double calculate_bandwidth(BandwidthPass elem) override;
    int insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr, int index) override;
    void delete_entry(uint64_t addr, uint64_t length) override;
    std::string output() override;
    virtual std::tuple<double, std::vector<uint64_t>> calculate_congestion();
    void set_epoch(int epoch) override;
};

#endif // CXLMEMSIM_CXLENDPOINT_H
