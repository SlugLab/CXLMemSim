//
// Created by victoryang00 on 1/13/23.
//

#ifndef CXL_MEM_SIMULATOR_CXLENDPOINT_H
#define CXL_MEM_SIMULATOR_CXLENDPOINT_H
#include "cxlcounter.h"
#include "helper.h"
class CXLEndPoint {
    virtual std::string output() = 0;
    virtual void delete_entry(uint64_t addr) = 0;
    virtual double calculate_latency(LatencyPass elem) = 0; // traverse the tree to calculate the latency
    virtual double calculate_bandwidth(BandwidthPass elem) = 0;
    virtual int insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr, int index) = 0; // 0 not this endpoint, 1 store, 2 load, 3 prefetch
};

class CXLMemExpander : public CXLEndPoint {
public:
    EmuCXLBandwidth bandwidth;
    EmuCXLLatency latency;
    uint64_t capacity;
    std::map<uint64_t, uint64_t> occupation;
    std::map<uint64_t, uint64_t> va_pa_map;
    CXLMemExpanderEvent counter{};
    uint64_t last_timestamp = 0;
    int id = -1;
    CXLMemExpander(int read_bw, int write_bw, int read_lat, int write_lat, int id);
    uint64_t va_to_pa(uint64_t addr);
    int insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr, int index) override;
    double calculate_latency(LatencyPass elem) override; // traverse the tree to calculate the latency
    double calculate_bandwidth(BandwidthPass elem) override;
    void delete_entry(uint64_t addr) override;
    std::string output() override;
};
class CXLSwitch : public CXLEndPoint {
public:
    std::vector<CXLMemExpander *> expanders{};
    std::vector<CXLSwitch *> switches{};
    CXLSwitchEvent counter{};
    int id = -1;
    uint64_t last_timestamp = 0;
    double congestion_latency = 0.02;
    explicit CXLSwitch(int id);
    double calculate_latency(LatencyPass elem) override; // traverse the tree to calculate the latency
    double calculate_bandwidth(BandwidthPass elem) override;
    int insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr, int index) override;
    void delete_entry(uint64_t addr) override;
    std::string output() override;
    virtual std::tuple<double,std::vector<uint64_t>> calculate_congestion();
};

#endif // CXL_MEM_SIMULATOR_CXLENDPOINT_H
