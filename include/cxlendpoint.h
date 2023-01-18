//
// Created by victoryang00 on 1/13/23.
//

#ifndef CXL_MEM_SIMULATOR_CXLENDPOINT_H
#define CXL_MEM_SIMULATOR_CXLENDPOINT_H
#include "cxlcounter.h"
#include "helper.h"
class CXLEndPoint {
    virtual void output();
};

class CXLMemExpander : public CXLEndPoint {
public:
    EmuCXLBandwidth bandwidth;
    EmuCXLLatency latency;
    uint64_t capacity;
    std::map<uint64_t, uint64_t> occupation;
    std::vector<uint64_t> lazy_remove;

    int id = -1;
    CXLMemExpander(int read_bw, int write_bw, int read_lat, int write_lat, int id);
    double calculate_latency(double weight, struct Elem *elem); // traverse the tree to calculate the latency
    double calculate_bandwidth(double weight, struct Elem *elem);
    void delete_entry(uint64_t addr);
    void insert(uint64_t timestamp, uint64_t size);
    void add_lazy_add(uint64_t addr);
    void add_lazy_remove(uint64_t addr);
    void va_to_pa();
    void output() override;
};
class CXLSwitch : public CXLEndPoint {
public:
    std::vector<CXLMemExpander *> expanders{};
    std::vector<CXLSwitch *> switches{};
    CXLCounter counter;
    int id = -1;
    explicit CXLSwitch(int id);
    void output() override;
};

#endif // CXL_MEM_SIMULATOR_CXLENDPOINT_H
