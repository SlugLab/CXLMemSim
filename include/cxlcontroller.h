//
// Created by victoryang00 on 1/14/23.
//

#ifndef CXLMEMSIM_CXLCONTROLLER_H
#define CXLMEMSIM_CXLCONTROLLER_H

#include "cxlcounter.h"
#include "cxlendpoint.h"
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

enum page_type { CACHELINE, PAGE, HUGEPAGE_2M, HUGEPAGE_1G };

class CXLController;
class AllocationPolicy {
public:
    AllocationPolicy();
    virtual int compute_once(CXLController *) = 0;
    // No write problem
};
class MigrationPolicy {
public:
    MigrationPolicy();
    virtual int compute_once(CXLController *) = 0; // reader writer
    // paging related
    // switching related
};

// need to give a timeout and will be added latency later,
class PagingPolicy {
public:
    PagingPolicy();
    virtual int compute_once(CXLController *) = 0; // reader writer
    // paging related
};

class CXLController : CXLSwitch {
public:
    std::vector<CXLMemExpander *> cur_expanders{};
    int capacity; // GB
    AllocationPolicy *policy;
    CXLCounter counter;
    std::map<uint64_t, uint64_t> occupation;
    std::map<uint64_t, uint64_t> va_pa_map;
    enum page_type page_type_; // percentage
    int num_switches = 0;

    CXLController(AllocationPolicy *p, int capacity, enum page_type page_type_, int epoch);
    void construct_topo(std::string_view newick_tree);
    void insert_end_point(CXLMemExpander *end_point);
    std::vector<std::string> tokenize(const std::string_view &s);
    std::tuple<double, std::vector<uint64_t>> calculate_congestion() override;
    void set_epoch(int epoch) override;
    std::tuple<int, int> get_all_access() override;
    double calculate_latency(LatencyPass elem) override; // traverse the tree to calculate the latency
    double calculate_bandwidth(BandwidthPass elem) override;
    int insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr, int index) override;
    void delete_entry(uint64_t addr, uint64_t length) override;
    std::string output() override;
};

#endif // CXLMEMSIM_CXLCONTROLLER_H
