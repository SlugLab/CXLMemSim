//
// Created by victoryang00 on 1/14/23.
//

#ifndef CXL_MEM_SIMULATOR_CXLCONTROLLER_H
#define CXL_MEM_SIMULATOR_CXLCONTROLLER_H

#include "cxlcounter.h"
#include "cxlendpoint.h"
#include "policy.h"
#include <cstdint>
#include <vector>

class CXLController {
public:
    std::vector<CXLEndPoint *> end_points{};
    CXLCounter *counter;
    Policy policy;
    int num_switches;
    CXLController(Policy policy);
    void construct_one(char *newick_tree, int &index, int end, CXLEndPoint *node);
    void construct_topo(std::string newick_tree);
    void insert_end_point(CXLEndPoint *end_point);
    double calculate_latency(double weight, struct Elem *elem); // traverse the tree to calculate the latency
    double calculate_bandwidth(double weight, struct Elem *elem);
    void delete_entry(uint64_t addr);
    void print();
}

#endif // CXL_MEM_SIMULATOR_CXLCONTROLLER_H
