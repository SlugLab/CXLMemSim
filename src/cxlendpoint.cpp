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

#include "cxlendpoint.h"
#include <random>

CXLMemExpander::CXLMemExpander(int read_bw, int write_bw, int read_lat, int write_lat, int id, int capacity)
    : capacity(capacity), id(id) {
    this->bandwidth.read = read_bw;
    this->bandwidth.write = write_bw;
    this->latency.read = read_lat;
    this->latency.write = write_lat;
}
double CXLMemExpander::calculate_latency(LatencyPass lat) {
    auto all_access = lat.all_access;
    auto dramlatency = lat.dramlatency;
    auto ma_ro = lat.readonly;
    auto ma_wb = lat.writeback;
    auto all_read = std::get<0>(all_access);
    auto all_write = std::get<1>(all_access);
    double read_sample = 0.;
    if (all_read != 0) {
        read_sample = (double)last_read / all_read;
    }
    double write_sample = 0.;
    if (all_write != 0) {
        write_sample = (double)last_write / all_write;
    }
    this->last_latency =
        ma_ro * read_sample * (latency.read - dramlatency) + ma_wb * write_sample * (latency.write - dramlatency);
    return this->last_latency;
}
double CXLMemExpander::calculate_bandwidth(BandwidthPass bw) {
    // Iterate the map within the last 20ms
    auto all_access = bw.all_access;
    auto read_config = bw.read_config;
    auto write_config = bw.write_config;

    double res = 0.0;
    auto all_read = std::get<0>(all_access);
    auto all_write = std::get<1>(all_access);
    double read_sample = 0.;
    if (all_read != 0) {
        read_sample = ((double)last_read / all_read);
    }
    double write_sample = 0.; // based on time series
    if (all_write != 0) {
        write_sample = ((double)last_write / all_write);
    }
    if (read_sample * 64 * read_config / 1024 / 1024 / (this->epoch + this->last_latency) * 1000 >
        bandwidth.read) {
        res +=
            read_sample * 64 * read_config / 1024 / 1024 / (this->epoch + this->last_latency) * 1000 / bandwidth.read -
            this->epoch * 0.001; // TODO: read
    }
    if (write_sample * 64 * write_config / 1024 / 1024 / (this->epoch + this->last_latency) * 1000 >
        bandwidth.write) {
        res += write_sample * 64 * write_config / 1024 / 1024 / (this->epoch + this->last_latency) * 1000 /
                bandwidth.write -
               this->epoch * 0.001; // TODO: wb+clflush
    }
    return res;
}
void CXLMemExpander::delete_entry(uint64_t addr, uint64_t length) {
    for (auto it1 = va_pa_map.begin(); it1 != va_pa_map.end();) {
        if (it1->second >= addr && it1->second <= addr + length) {
            for (auto it = occupation.begin(); it != occupation.end();) {
                if (it->second == addr) {
                    it = occupation.erase(it);
                } else {
                    ++it;
                }
            }
            it1 = va_pa_map.erase(it1);
            this->counter.inc_load();
        }
    }
    // kernel mode access
    for (auto it = occupation.begin(); it != occupation.end();) {
        if (it->second >= addr && it->second <= addr + length) {
            if (it->second == addr) {
                it = occupation.erase(it);
            } else {
                ++it;
            }
        }
        this->counter.inc_load();
    }
}

int CXLMemExpander::insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr, int index) {

    if (index == this->id) {
        last_timestamp = last_timestamp > timestamp ? last_timestamp : timestamp; // Update the last timestamp
        // Check if the address is already in the map)
        if (phys_addr != 0) {
            if (va_pa_map.find(virt_addr) == va_pa_map.end()) {
                this->va_pa_map.emplace(virt_addr, phys_addr);
            } else {
                this->va_pa_map[virt_addr] = phys_addr;
                SPDLOG_DEBUG("virt:{} phys:{} conflict insertion detected\n", virt_addr, phys_addr);
            }
            for (auto it = this->occupation.cbegin(); it != this->occupation.cend(); it++) {
                if ((*it).second == phys_addr) {
                    this->occupation.erase(it);
                    this->occupation.emplace(timestamp, phys_addr);
                    this->counter.inc_load();
                    return 2;
                }
            }
            this->occupation.emplace(timestamp, phys_addr);
            this->counter.inc_store();
            return 1;
        } // kernel mode access
        for (auto it = this->occupation.cbegin(); it != this->occupation.cend(); it++) {
            if ((*it).second == virt_addr) {
                this->occupation.erase(it);
                this->occupation.emplace(timestamp, virt_addr);
                this->counter.inc_load();
                return 2;
            }
        }

        this->occupation.emplace(timestamp, virt_addr);
        this->counter.inc_store();
        return 1;
    }
    return 0;
}
std::string CXLMemExpander::output() { return std::format("CXLMemExpander {}", this->id); }
std::tuple<int, int> CXLMemExpander::get_all_access() {
    this->last_read = this->counter.load - this->last_counter.load;
    this->last_write = this->counter.store - this->last_counter.store;
    last_counter = CXLMemExpanderEvent(counter);
    return std::make_tuple(this->last_read, this->last_write);
}
void CXLMemExpander::set_epoch(int epoch) { this->epoch = epoch; }
void CXLMemExpander::free_stats(double size) {
    std::vector<uint64_t> keys;
    for (auto &it : this->va_pa_map) {
        keys.push_back(it.first);
    }
    std::shuffle(keys.begin(), keys.end(), std::mt19937(std::random_device()()));
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        if (this->va_pa_map[*it] > size) {
            this->va_pa_map.erase(*it);
            this->occupation.erase(*it);
            this->counter.inc_load();
        }
    }
}

std::string CXLSwitch::output() {
    std::string res = std::format("CXLSwitch {} ", this->id);
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
    // time series
    return bw;
}
int CXLSwitch::insert(uint64_t timestamp, uint64_t tid, struct lbr *lbrs, struct cntr *counters) {
    // 这里可以根据你的功能逻辑来处理 LBR 的插入信息
    SPDLOG_DEBUG("CXLSwitch insert lbr for switch id:{}\n", this->id);

    // 简单示例: 依次尝试调用下属的 Expander 和 Switch
    for (auto &expander : this->expanders) {
        int ret = expander->insert(timestamp, tid, lbrs, counters);
        if (ret != 0) {
            // 如果需要，执行相应的 load/store 计数
            this->counter.inc_load();
            return ret;
        }
    }
    for (auto &sw : this->switches) {
        int ret = sw->insert(timestamp, tid, lbrs, counters);
        if (ret != 0) {
            this->counter.inc_load();
            return ret;
        }
    }
    return 0; // 未找到合适的处理者
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
                congestion.push_back(it.first);
            }
        }
    }
    sort(congestion.begin(), congestion.end());
    for (auto it = congestion.begin(); it != congestion.end(); ++it) {
        if (*(it + 1) - *it < 2000) { // if less than 20ns
            latency += this->congestion_latency;
            this->counter.inc_conflict();
            if (it + 1 == congestion.end()) {
                break;
            }
            congestion.erase(it);
        }
    }
    return std::make_tuple(latency, congestion);
}
std::tuple<int, int> CXLSwitch::get_all_access() {
    int read = 0, write = 0;
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
void CXLSwitch::free_stats(double size) {
    // 随机删除
    for (auto &expander : this->expanders) {
        expander->free_stats(size);
    }
}

int CXLMemExpander::insert(uint64_t timestamp, uint64_t tid, struct lbr *lbrs, struct cntr *counters) {
    // 这里可以根据你的功能逻辑来处理 LBR 的插入信息
    SPDLOG_DEBUG("CXLMemExpander insert lbr for expander id:{}\n", this->id);

    // 简单示例: 统计一次 load 或 store
    this->counter.inc_load();
    // 或者根据需要添加更多逻辑

    return 1; // 返回非 0，表明已被当前 Expander 处理
}

int CXLSwitch::insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr, int index) {
    // 简单示例：依次调用下属的 expander 和 switch
    SPDLOG_DEBUG("CXLSwitch insert phys_addr={}, virt_addr={}, index={} for switch id:{}", phys_addr, virt_addr, index,
                 this->id);

    for (auto &expander : this->expanders) {
        // 在每个 expander 上尝试插入
        int ret = expander->insert(timestamp, phys_addr, virt_addr, index);
        if (ret == 1) {
            this->counter.inc_store();
            return 1;
        }
        if (ret == 2) {
            this->counter.inc_load();
            return 2;
        }
    }
    // 如果没有合适的 expander，就尝试下属的 switch
    for (auto &sw : this->switches) {
        int ret = sw->insert(timestamp, phys_addr, virt_addr, index);
        if (ret == 1) {
            this->counter.inc_store();
            return 1;
        }
        if (ret == 2) {
            this->counter.inc_load();
            return 2;
        }
    }
    // 如果都处理不了，就返回0
    return 0;
}
