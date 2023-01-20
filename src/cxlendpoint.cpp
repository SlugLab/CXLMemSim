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
        this->counter.inc_load();
    }
}
uint64_t CXLMemExpander::va_to_pa(uint64_t addr) {
    if (va_pa_map.find(addr) != va_pa_map.end()) {
        auto phys = va_pa_map[addr];
        va_pa_map.erase(addr);
        return phys;
    }
    return -1;
}

int CXLMemExpander::insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr, int index) {
    if (index == this->id) {
        if (va_pa_map.find(virt_addr) != va_pa_map.end()) {
            this->va_pa_map.emplace(virt_addr, phys_addr);
        } else {
            this->va_pa_map[virt_addr] = phys_addr;
            LOG(INFO) << fmt::format("virt:{} phys:{} conflict insertion detected", virt_addr, phys_addr);
        }
        for (auto it = this->occupation.cbegin(); it != this->occupation.cend(); it++) {
            if ((*it).second == phys_addr) {
                this->occupation.emplace(timestamp, phys_addr);
                this->counter.inc_store();
            } else {
                this->occupation.erase(it);
                this->occupation.emplace(timestamp, phys_addr);
                this->counter.inc_load();
            }
        }
        return true;
    } else {
        return false;
    }
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
CXLSwitch::CXLSwitch(int id) : id(id) {}
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
int CXLSwitch::insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr, int index) {
    for (auto &expander : this->expanders) {
        if (expander->insert(timestamp, phys_addr, virt_addr, index)) {
            this->counter.inc_store();
            return true;
        };
    }
    for (auto &switch_ : this->switches) {
        switch_->insert(timestamp, phys_addr, virt_addr, index);
    }
}
std::tuple<double, std::vector<uint64_t>> CXLSwitch::calculate_congestion() {
    double latency = 0.0;
    std::vector<uint64_t> congestion;
    for (auto &switch_ : this->switches) {
        auto [lat, con] = switch_->calculate_congestion();
        latency += lat;
        congestion.insert(congestion.end(), con.begin(), con.end());
    }
    for (auto &expander : this->expanders) {
        for (auto &it : expander->occupation) {
            // 20ms every epoch
            if (it.first > this->last_timestamp - 20000) {
                congestion.push_back(it.second);
            }
        }
    }
    sort(congestion.begin(), congestion.end());
    for (auto it = congestion.begin(); it != congestion.end(); ++it) {
        if (*(it + 1) - *it < 20) {
            latency += this->congestion_latency;
            this->counter.inc_conflict();
            congestion.erase(it);
        }
    }
}
