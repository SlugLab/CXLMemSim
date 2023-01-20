//
// Created by victoryang00 on 1/13/23.
//

#include "cxlendpoint.h"
#define PAGEMAP_LENGTH 8
CXLMemExpander::CXLMemExpander(int read_bw, int write_bw, int read_lat, int write_lat, int id) {
    this->bandwidth.read = read_bw;
    this->bandwidth.write = write_bw;
    this->latency.read = read_lat;
    this->latency.write = write_lat;
    this->id = id;
}
double CXLMemExpander::calculate_latency(LatencyPass elem) { return 0; }
double CXLMemExpander::calculate_bandwidth(BandwidthPass elem) {
    // Iterate the map within the last 20ms

    return 0;
}
void CXLMemExpander::delete_entry(uint64_t addr) {
    if (occupation.find(addr) != occupation.end()) {
        occupation.erase(addr);
    }
}
uint64_t CXLMemExpander::va_to_pa(uint64_t addr) {
    // FILE *pagemap = fopen(fmt::format("/proc/{}/pagemap", pid), "rb");
    //
    // unsigned long offset = (unsigned long)addr / getpagesize() * PAGEMAP_LENGTH;
    // if (fseek(pagemap, (unsigned long)offset, SEEK_SET) != 0) {
    //     LOG(ERROR) << "Failed to seek pagemap to proper location\n";
    //     exit(1);
    // }
    //
    // unsigned long page_frame_number = 0;
    // fread(&page_frame_number, 1, PAGEMAP_LENGTH - 1, pagemap);
    //
    // page_frame_number &= 0x7FFFFFFFFFFFFF;
    //
    // fclose(pagemap);
    // return page_frame_number;
    if (va_pa_map.find(addr) != va_pa_map.end()) {
        auto phys = va_pa_map[addr];
        va_pa_map.erase(addr);
        return phys;
    }
    return -1;
}
void CXLMemExpander::add_lazy_remove(uint64_t addr) { this->lazy_remove.push_back(va_to_pa(addr)); }

bool CXLMemExpander::insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr) {
    this->va_pa_map.emplace(virt_addr, phys_addr);
    this->occupation.emplace(timestamp, phys_addr);
}
std::string CXLMemExpander::output() { return fmt::format("CXLMemExpander {}", this->id); }
std::string CXLSwitch::output() {
    std::string res = fmt::format("CXLSwitch {} ", this->id);
    if (!this->switches.empty()) {
        res += "(";
        res += this->switches[0]->output();
        for (size_t i = 1; i < this->switches.size(); ++i) {
            res += ",";
            res += this->switches[i]->output();
        }
        res += ")";
    } else if (!this->expanders.empty()) {
        res += "(";
        res += this->expanders[0]->output();
        for (size_t i = 1; i < this->expanders.size(); ++i) {
            res += ",";
            res += this->expanders[i]->output();
        }
        res += ")";
    }
    return res;
}
void CXLSwitch::delete_entry(uint64_t addr) {
    for (auto &expander : this->expanders) {
        expander->delete_entry(addr);
    }
    for (auto &switch_ : this->switches) {
        switch_->delete_entry(addr);
    }
}
CXLSwitch::CXLSwitch(int id) : id(id) { this->counter = CXLCounter(); }
double CXLSwitch::calculate_latency(LatencyPass elem) {
    double lat = 0.0;
    for (auto &expander : this->expanders) {
        lat += expander->calculate_latency(elem);
    }
    for (auto &switch_ : this->switches) {
        lat += switch_->calculate_latency(elem);
    }
    return lat;
}
double CXLSwitch::calculate_bandwidth(BandwidthPass elem) {
    double bw = 0.0;
    for (auto &expander : this->expanders) {
        bw += expander->calculate_bandwidth(elem);
    }
    for (auto &switch_ : this->switches) {
        bw += switch_->calculate_bandwidth(elem);
    }
    return bw;
}
bool CXLSwitch::insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr) {
    for (auto &expander : this->expanders) {
        expander->insert(timestamp, phys_addr, virt_addr);
    }
    for (auto &switch_ : this->switches) {
        switch_->insert(timestamp, phys_addr, virt_addr);
    }
}
