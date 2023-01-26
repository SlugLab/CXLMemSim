//
// Created by victoryang00 on 1/13/23.
//

#include "cxlendpoint.h"

CXLMemExpander::CXLMemExpander(int read_bw, int write_bw, int read_lat, int write_lat, int id) {
    this->bandwidth.read = read_bw;
    this->bandwidth.write = write_bw;
    this->latency.read = read_lat;
    this->latency.write = write_lat;
    this->id = id;
}
double CXLMemExpander::calculate_latency(LatencyPass lat) {
    auto all_access = lat.all_access;
    auto dramlatency = lat.dramlatency;
    auto ma_ro = lat.ma_ro;
    auto ma_wb = lat.ma_wb;
    auto read_sample = last_read / std::get<0>(all_access);
    auto write_sample = last_write / std::get<1>(all_access);
    return ma_ro * read_sample * (latency.read - dramlatency) + ma_wb * write_sample * (latency.write - dramlatency);
}
double CXLMemExpander::calculate_bandwidth(BandwidthPass bw) {
    // Iterate the map within the last 20ms
    auto all_access = bw.all_access;
    auto read_config = bw.read_config;
    auto write_config = bw.write_config;
    auto read_sample = last_read / std::get<0>(all_access);
    auto write_sample = last_write / std::get<1>(all_access);
    double res = 0.0;
    if (read_sample * 64 * read_config / 1024 / 1024 * 50 > bandwidth.read) {
        res += read_sample * 64 * read_config / 1024 / 1024 * 50 / bandwidth.read - 0.02;
    }
    if (write_sample * 64 * write_config / 1024 / 1024 * 50 > bandwidth.write) {
        res += write_sample * 64 * write_config / 1024 / 1024 * 50 / bandwidth.write - 0.02;
    }
    return res;
}
void CXLMemExpander::delete_entry(uint64_t addr, uint64_t length) {
    for (auto it = va_pa_map.begin(); it != va_pa_map.end();) {
        if (it->second >= addr && it->second <= addr + length) {
            for (auto it = occupation.begin(); it != occupation.end();) {
                if (it->second == addr) {
                    it = occupation.erase(it);
                } else {
                    it++;
                }
            }
            it = va_pa_map.erase(it);
            this->counter.inc_load();
        }
    }
}

int CXLMemExpander::insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr, int index) {
    if (index == this->id) {
        if (va_pa_map.find(virt_addr) != va_pa_map.end()) {
            this->va_pa_map.emplace(virt_addr, phys_addr);
        } else {
            this->va_pa_map[virt_addr] = phys_addr;
            LOG(INFO) << fmt::format("virt:{} phys:{} conflict insertion detected", virt_addr, phys_addr);
        }
        last_timestamp = last_timestamp > timestamp ? last_timestamp : timestamp; // Update the last timestamp
        // Check if the address is already in the map
        for (auto it = this->occupation.cbegin(); it != this->occupation.cend(); it++) {
            if ((*it).second == phys_addr) {
                this->occupation.emplace(timestamp, phys_addr);
                this->counter.inc_store();
                return 1;
            } else {
                this->occupation.erase(it);
                this->occupation.emplace(timestamp, phys_addr);
                this->counter.inc_load();
                return 2;
            }
        }
    } else {
        return 0;
    }
}
std::string CXLMemExpander::output() { return fmt::format("CXLMemExpander {}", this->id); }
std::tuple<int, int> CXLMemExpander::get_all_access() {
    this->last_read = this->counter.load - this->last_counter.load;
    this->last_write = this->counter.store - this->last_counter.store;
    last_counter = CXLMemExpanderEvent(counter);
    return std::make_tuple(this->last_read, this->last_write);
}
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
void CXLSwitch::delete_entry(uint64_t addr, uint64_t length) {
    for (auto &expander : this->expanders) {
        expander->delete_entry(addr, length);
    }
    for (auto &switch_ : this->switches) {
        switch_->delete_entry(addr, length);
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
        auto ret = expander->insert(timestamp, phys_addr, virt_addr, index);
        if (ret == 1) {
            this->counter.inc_store();
            return 1;
        } else if (ret == 2) {
            this->counter.inc_load();
            return 2;
        } else {
            return 0;
        };
    }
    for (auto &switch_ : this->switches) {
        auto ret = switch_->insert(timestamp, phys_addr, virt_addr, index);
        if (ret == 1) {
            this->counter.inc_store();
            return 1;
        } else if (ret == 2) {
            this->counter.inc_load();
            return 2;
        } else {
            return 0;
        };
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
            // every epoch
            if (it.first > this->last_timestamp - epoch * 1e3) {
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
    return std::make_tuple(latency, congestion);
}
std::tuple<int, int> CXLSwitch::get_all_access() {
    int read, write;
    for (auto &expander : this->expanders) {
        auto [r, w] = expander->get_all_access();
        read += r;
        write += w;
    }
    for (auto &switch_ : this->switches) {
        auto [r, w] = switch_->get_all_access();
        read += r;
        write += w;
    }
    return std::make_tuple(read, write);
}
void CXLSwitch::set_epoch(int epoch) { this->epoch = epoch; }
