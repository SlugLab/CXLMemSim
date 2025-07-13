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
#include <algorithm>

CXLMemExpander::CXLMemExpander(int read_bw, int write_bw, int read_lat, int write_lat, int id, int capacity)
    : capacity(capacity), id(id), read_credits_(INITIAL_CREDITS), write_credits_(INITIAL_CREDITS) {
    this->bandwidth.read = read_bw;
    this->bandwidth.write = write_bw;
    this->latency.read = read_lat;
    this->latency.write = write_lat;
}
// 修改CXLMemExpander的calculate_latency函数
double CXLMemExpander::calculate_latency(const std::vector<std::tuple<uint64_t, uint64_t>> &elem, double dramlatency) {
    if (elem.empty()) {
        return 0.0;
    }

    // Process any pending requests first
    if (!elem.empty()) {
        process_queued_requests(std::get<0>(elem.back()));
    }

    // 首先更新地址缓存以确保其最新
    update_address_cache();

    double total_latency = 0.0;
    size_t access_count = 0;
    
    // Track pipeline benefits
    double pipeline_benefit = 0.0;
    uint64_t last_timestamp = 0;

    for (const auto &[timestamp, addr] : elem) {
        // 使用哈希表检查是否是本endpoint的访问
        bool is_local_access = is_address_local(addr);

        if (!is_local_access)
            continue;

        // Check if request is in flight (pipeline processing)
        bool is_pipelined = false;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            is_pipelined = in_flight_requests_.find(addr) != in_flight_requests_.end();
        }

        // Create a temporary request for latency calculation
        CXLRequest temp_req;
        temp_req.timestamp = timestamp;
        temp_req.address = addr;
        temp_req.is_read = true;  // Assume read for now
        temp_req.is_write = false;

        // Calculate latency with pipeline modeling
        double current_latency = 0.0;
        
        if (is_pipelined) {
            // Request is already in pipeline, reduced latency
            current_latency = this->latency.read * 0.3;  // 70% reduction due to pipeline
            pipeline_benefit += this->latency.read * 0.7;
        } else {
            // Full pipeline latency calculation
            current_latency = calculate_pipeline_latency(temp_req);
            
            // Check for pipeline overlap with previous requests
            if (last_timestamp > 0 && (timestamp - last_timestamp) < 100) {
                // Requests are close in time, benefit from pipeline
                double overlap_ratio = 1.0 - ((timestamp - last_timestamp) / 100.0);
                double overlap_benefit = current_latency * overlap_ratio * 0.5;
                current_latency -= overlap_benefit;
                pipeline_benefit += overlap_benefit;
            }
        }

        // 考虑DRAM延迟影响
        current_latency += dramlatency * 0.1;

        total_latency += current_latency;
        access_count++;
        last_timestamp = timestamp;
    }

    // Apply queue state benefits
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (request_queue_.size() < MAX_QUEUE_SIZE / 4) {
            // Low queue utilization, faster processing
            total_latency *= 0.9;
        }
    }

    return access_count > 0 ? total_latency / access_count : 0.0;
}

double CXLMemExpander::calculate_bandwidth(const std::vector<std::tuple<uint64_t, uint64_t>> &elem) {
    if (elem.empty()) {
        return 0.0;
    }

    // 获取20ms时间窗口内的访问
    uint64_t current_time = std::get<0>(elem.back());
    uint64_t window_start = current_time - 20000000; // 20ms = 20,000,000ns

    // 计算时间窗口内的访问次数
    size_t access_count = 0;
    uint64_t total_data = 0;
    constexpr uint64_t CACHE_LINE_SIZE = 64; // 假设缓存行大小为64字节

    for (const auto &[timestamp, addr] : elem) {
        if (timestamp >= window_start) {
            access_count++;
            total_data += CACHE_LINE_SIZE; // TODO other than cacheline granularity
        }
    }

    // 计算带宽 (GB/s)
    // 带宽 = (总数据量 / 时间窗口)
    double time_window_seconds = 0.02; // 20ms = 0.02s
    double bandwidth_gbps = (total_data / time_window_seconds) / (1024.0 * 1024.0 * 1024.0);

    // 确保带宽不超过设备限制
    double max_bandwidth = this->bandwidth.read + this->bandwidth.write;
    return std::min(bandwidth_gbps- max_bandwidth, 0.0);
}
void CXLMemExpander::delete_entry(uint64_t addr, uint64_t length) {
    bool modified = false;

    // 使用引用来确保可以修改 occupation 中的元素
    for (auto& occ : occupation) {
        if (occ.address == addr) {
            occ.access_count++;
            occ.timestamp = last_timestamp;
            modified = true;
        }
    }

    // kernel mode access
    this->counter.inc_load();  // 只调用一次而不是每次迭代
    for (auto& occ : occupation) {
        if (occ.address >= addr && occ.address <= addr + length) {
            if (occ.address == addr) {
                continue;  // 已经在上面处理过
            }
            occ.access_count++;
            occ.timestamp = last_timestamp;
            modified = true;
        }
    }

    // 如果有修改，标记缓存为无效
    if (modified) {
        invalidate_cache();
    }
}

int CXLMemExpander::insert(uint64_t timestamp, uint64_t tid, uint64_t phys_addr, uint64_t virt_addr, int index) {
    if (index == this->id) {
        last_timestamp = last_timestamp > timestamp ? last_timestamp : timestamp;
        
        // Process any completed requests
        process_queued_requests(timestamp);

        if (phys_addr != 0) {
            // Create a new CXL request
            CXLRequest req;
            req.timestamp = timestamp;
            req.address = phys_addr;
            req.tid = tid;
            req.is_read = false;  // Default to write, will be updated based on operation
            req.is_write = true;
            
            // Check if queue can accept the request
            if (!can_accept_request()) {
                // Queue is full, reject the request
                // Note: CXLMemExpanderEvent doesn't have inc_conflict, just count as hit_old
                this->counter.inc_hit_old();
                return 0;
            }
            
            // Add request to queue
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                request_queue_.push_back(req);
            }
            
            // 使用哈希表快速检查地址是否已存在
            bool address_exists = address_cache.find(phys_addr) != address_cache.end();

            if (address_exists) {
                // 地址已存在，找到并更新
                for (auto it = this->occupation.cbegin(); it != this->occupation.cend(); it++) {
                    if (it->address == phys_addr) {
                        this->occupation.erase(it);
                        this->occupation.emplace_back(timestamp, phys_addr, 0);
                        this->counter.inc_load();

                        // Update request type
                        request_queue_.back().is_read = true;
                        request_queue_.back().is_write = false;
                        
                        // 不需要更新缓存，地址没变
                        return 2;
                    }
                }
            }

            // 地址不存在，添加新条目
            this->occupation.emplace_back(timestamp, phys_addr, 0);

            // 更新地址缓存
            address_cache.insert(phys_addr);

            this->counter.inc_store();
            return 1;
        }
        this->counter.inc_store();
        return 1;
    }
    return 0;
}
std::vector<std::tuple<uint64_t, uint64_t>> CXLMemExpander::get_access(uint64_t timestamp) {
    // 原子操作更新计数器
    last_counter = CXLMemExpanderEvent(counter);

    // 使用互斥锁保护对共享资源的访问

    // 创建一个本地副本，减少持锁时间
    std::vector<std::tuple<uint64_t, uint64_t>> result;
    for (const auto &it : occupation) {
        // 如果 occupation 中的元素是指针，需要先检查指针有效性
        if (it.timestamp > timestamp - 100000) {
            result.emplace_back(it.timestamp, it.address);
        }
    }
    return result;
}
void CXLMemExpander::set_epoch(int epoch) { this->epoch = epoch; }
void CXLMemExpander::free_stats(double size) {
    bool modified = false;

    // 随机删除
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 1);
    for (auto it = occupation.begin(); it != occupation.end();) {
        if (dis(gen) == 1) {
            it = occupation.erase(it);
            modified = true;
        } else {
            ++it;
        }
    }

    // 如果有修改，重建地址缓存
    if (modified) {
        invalidate_cache();
    }
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
double CXLSwitch::calculate_latency(const std::vector<std::tuple<uint64_t, uint64_t>> &elem, double dramlatency) {
    double lat = 0.0;
    for (auto &expander : this->expanders) {
        lat += expander->calculate_latency(elem, dramlatency);
    }
    for (auto &switch_ : this->switches) {
        lat += switch_->calculate_latency(elem, dramlatency);
    }
    return lat;
}
double CXLSwitch::calculate_bandwidth(const std::vector<std::tuple<uint64_t, uint64_t>> &elem) {
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
double CXLSwitch::get_endpoint_rob_latency(CXLMemExpander *endpoint,
                                         const std::vector<std::tuple<uint64_t, uint64_t>> &accesses,
                                         const thread_info &t_info, double dramlatency) {
    const auto &rob = t_info.rob;

    // 计算当前endpoint的基础延迟
    double base_latency = (endpoint->latency.read + endpoint->latency.write) / 2.0;

    // 计算ROB相关指标
    double llc_miss_ratio = (rob.ins_count > 0) ? static_cast<double>(rob.llcm_count) / rob.ins_count : 0.0;

    // 使用find替代operator[]来访问const map
    double remote_ratio = 0.0;
    auto count_0 = rob.m_count.find(0);
    auto count_1 = rob.m_count.find(1);

    if (count_0 != rob.m_count.end() && count_1 != rob.m_count.end()) {
        int64_t local_count = count_0->second;
        int64_t remote_count = count_1->second;
        if (local_count + remote_count > 0) {
            remote_ratio = static_cast<double>(remote_count) / (local_count + remote_count);
        }
    }

    // 确保地址缓存是最新的
    endpoint->update_address_cache();

    double total_latency = 0.0;
    size_t access_count = 0;

    for (const auto &[timestamp, addr] : accesses) {
        // 使用哈希表快速检查地址是否属于这个endpoint
        bool is_endpoint_access = endpoint->is_address_local(addr);

        if (is_endpoint_access)
            continue;

        double current_latency = base_latency;
        access_count++;

        // ROB拥塞调整
        if (rob.ins_count >= ROB_SIZE * 0.8) {
            double rob_penalty = 1.0 + (llc_miss_ratio * 0.5);
            current_latency *= rob_penalty;
        }

        // 远程访问影响
        auto remote_count = rob.m_count.find(1);
        if (remote_count != rob.m_count.end() && remote_count->second > 0) {
            current_latency *= (1.0 + remote_ratio * 0.3);
        }

        // 考虑DRAM延迟
        current_latency += dramlatency * (remote_ratio + 0.1);

        total_latency += current_latency;
    }

    return access_count > 0 ? total_latency / access_count : 0.0;
}

std::tuple<double, std::vector<uint64_t>> CXLSwitch::calculate_congestion() {
    double latency = 0.0;
    std::vector<uint64_t> congestion;

    // 收集所有子交换机的拥塞信息
    for (auto &switch_ : this->switches) {
        auto [lat, con] = switch_->calculate_congestion();
        latency += lat;
        // 预先分配空间以避免多次重新分配
        congestion.reserve(congestion.size() + con.size());
        congestion.insert(congestion.end(), con.begin(), con.end());
    }

    // 使用哈希表来聚合地址访问信息，避免后续多次排序
    struct AccessInfo {
        std::vector<uint64_t> timestamps;
        std::vector<bool> is_write;
    };

    std::unordered_map<uint64_t, AccessInfo> address_map;
    uint64_t current_time = this->last_timestamp - epoch * 1000;

    // 一次性收集所有访问记录
    for (auto &expander : this->expanders) {
        for (auto &it : expander->occupation) {
            if (it.timestamp > current_time) {
                bool is_write = it.access_count == 0;

                // 直接添加到对应地址的记录中
                address_map[it.address].timestamps.push_back(it.timestamp);
                address_map[it.address].is_write.push_back(is_write);

                congestion.push_back(it.timestamp);
            }
        }
    }

    // 时间冲突检测 - 使用桶排序思想
    if (!congestion.empty()) {
        std::sort(congestion.begin(), congestion.end());

        // 使用单次遍历检测时间冲突
        auto it = congestion.begin();
        auto end = congestion.end() - 1;
        while (it < end) {
            if (*(it + 1) - *it < 2000) {
                latency += this->congestion_latency;
                this->counter.inc_conflict();
                // 使用交换和弹出末尾元素代替erase，避免移动元素
                std::swap(*it, *end);
                congestion.pop_back();
                end--;
                // 不更新it，因为新交换的元素也需要检查
            } else {
                ++it;
            }
        }
    }

    // 读写冲突检测 - 针对每个地址只排序一次
    for (auto& [address, info] : address_map) {
        auto& timestamps = info.timestamps;
        auto& is_write = info.is_write;

        if (timestamps.size() <= 1) continue;

        // 对时间戳和操作类型同时排序
        std::vector<std::pair<uint64_t, bool>> sorted_access;
        sorted_access.reserve(timestamps.size());

        for (size_t i = 0; i < timestamps.size(); ++i) {
            sorted_access.emplace_back(timestamps[i], is_write[i]);
        }

        std::sort(sorted_access.begin(), sorted_access.end());

        // 单次遍历检测冲突
        for (size_t i = 0; i < sorted_access.size() - 1; i++) {
            const auto& current = sorted_access[i];
            const auto& next = sorted_access[i + 1];

            if (next.first - current.first < 2000) {
                if (current.second || next.second) {
                    if (current.second && next.second) {
                        latency += this->congestion_latency * 2.0; // 写-写冲突
                    } else {
                        latency += this->congestion_latency * 1.5; // 读-写或写-读冲突
                    }
                    this->counter.inc_conflict();
                } else {
                    latency += this->congestion_latency * 0.5; // 读-读冲突
                }
            }
        }
    }

    return std::make_tuple(latency, congestion);
}

std::vector<std::tuple<uint64_t, uint64_t>> CXLSwitch::get_access(uint64_t timestamp) {
    std::vector<std::tuple<uint64_t, uint64_t>> res;
    size_t total_size = 0;
    for (const auto &expander : expanders) {
        total_size += expander->get_access(timestamp).size();
    }
    for (const auto &switch_ : switches) {
        total_size += switch_->get_access(timestamp).size();
    }
    res.reserve(total_size);

    // 直接使用 insert 合并结果
    for (auto &expander : expanders) {
        auto tmp = expander->get_access(timestamp);
        res.insert(res.end(), tmp.begin(), tmp.end());
    }
    for (auto &switch_ : switches) {
        auto tmp = switch_->get_access(timestamp);
        res.insert(res.end(), tmp.begin(), tmp.end());
    }

    return res;
}
void CXLSwitch::set_epoch(int epoch) { this->epoch = epoch; }
void CXLSwitch::free_stats(double size) {
    // 随机删除
    for (auto &expander : this->expanders) {
        expander->free_stats(size);
    }
}

int CXLSwitch::insert(uint64_t timestamp, uint64_t tid, uint64_t phys_addr, uint64_t virt_addr, int index) {
    // 简单示例：依次调用下属的 expander 和 switch
    SPDLOG_DEBUG("CXLSwitch insert phys_addr={}, virt_addr={}, index={} for switch id:{}", phys_addr, virt_addr, index,
                 this->id);

    for (auto &expander : this->expanders) {
        // 在每个 expander 上尝试插入
        int ret = expander->insert(timestamp, tid, phys_addr, virt_addr, index);
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
        int ret = sw->insert(timestamp, tid, phys_addr, virt_addr, index);
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

// Implementation of new CXL queue management and pipeline methods
bool CXLMemExpander::can_accept_request() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return request_queue_.size() < MAX_QUEUE_SIZE;
}

bool CXLMemExpander::has_credits(bool is_read) const {
    if (is_read) {
        return read_credits_.load() > 0;
    } else {
        return write_credits_.load() > 0;
    }
}

void CXLMemExpander::consume_credit(bool is_read) {
    if (is_read) {
        read_credits_.fetch_sub(1);
    } else {
        write_credits_.fetch_sub(1);
    }
}

void CXLMemExpander::release_credit(bool is_read) {
    if (is_read) {
        size_t current = read_credits_.load();
        if (current < INITIAL_CREDITS) {
            read_credits_.fetch_add(1);
        }
    } else {
        size_t current = write_credits_.load();
        if (current < INITIAL_CREDITS) {
            write_credits_.fetch_add(1);
        }
    }
}

double CXLMemExpander::calculate_pipeline_latency(const CXLRequest& req) {
    double total_latency = 0.0;
    
    // Frontend processing latency
    total_latency += frontend_latency_;
    
    // Forward path latency
    total_latency += forward_latency_;
    
    // Memory access latency (read or write)
    if (req.is_read) {
        total_latency += this->latency.read;
    } else {
        total_latency += this->latency.write;
    }
    
    // Response path latency
    total_latency += response_latency_;
    
    // Protocol overhead based on data size (assuming 64B cache line)
    total_latency += calculate_protocol_overhead(64);
    
    // Congestion delay based on queue occupancy
    total_latency += calculate_congestion_delay(req.timestamp);
    
    return total_latency;
}

void CXLMemExpander::process_queued_requests(uint64_t current_time) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    // Process requests that have completed their pipeline stages
    auto it = in_flight_requests_.begin();
    while (it != in_flight_requests_.end()) {
        if (it->second.complete_time <= current_time) {
            // Release credit when request completes
            release_credit(it->second.is_read);
            it = in_flight_requests_.erase(it);
        } else {
            ++it;
        }
    }
    
    // Try to issue new requests from queue
    while (!request_queue_.empty() && in_flight_requests_.size() < MAX_QUEUE_SIZE / 2) {
        CXLRequest& req = request_queue_.front();
        
        // Check if we have credits available
        if (!has_credits(req.is_read)) {
            break;  // Wait for credits
        }
        
        // Consume credit and move to in-flight
        consume_credit(req.is_read);
        req.issue_time = current_time;
        req.complete_time = current_time + calculate_pipeline_latency(req);
        
        in_flight_requests_[req.address] = req;
        request_queue_.pop_front();
    }
}

double CXLMemExpander::calculate_congestion_delay(uint64_t timestamp) {
    // Calculate congestion based on queue occupancy
    double queue_utilization = static_cast<double>(request_queue_.size()) / MAX_QUEUE_SIZE;

    // Non-linear congestion model
    if (queue_utilization < 0.5) {
        return 0.0;  // No congestion
    } else if (queue_utilization < 0.8) {
        return (queue_utilization - 0.5) * 20.0;  // Linear increase
    } else {
        return 6.0 + (queue_utilization - 0.8) * 100.0;  // Steep increase when nearly full
    }
}

double CXLMemExpander::calculate_protocol_overhead(size_t data_size) {
    // Calculate number of flits needed
    size_t num_flits = (data_size + FLIT_SIZE - 1) / FLIT_SIZE;
    
    // Add data flit overhead
    double overhead = num_flits * DATA_FLIT * 0.1;  // 0.1ns per byte overhead
    
    return overhead;
}
