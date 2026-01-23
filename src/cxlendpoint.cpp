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
#include "coherency_engine.h"
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

/* ============================================================================
 * MH-SLD (Multi-Headed Single Logical Device) Implementation
 *
 * Provides pooling and sharing of CXL memory across multiple hosts,
 * with cacheline-state-aware coherency protocol and LogP-based latency.
 * ============================================================================ */

MHSLDDevice::MHSLDDevice(uint64_t capacity, uint32_t num_heads, double read_lat,
                         double write_lat, double bandwidth, const LogPConfig& logp_cfg)
    : total_capacity(capacity), num_heads(num_heads),
      base_read_latency(read_lat), base_write_latency(write_lat),
      max_bandwidth(bandwidth), logp_model(logp_cfg) {

    heads.resize(MAX_HEADS);
    for (uint32_t i = 0; i < MAX_HEADS; i++) {
        heads[i].head_id = i;
        heads[i].active = false;
        heads[i].bandwidth_share = 0.0;
    }
}

bool MHSLDDevice::activate_head(uint32_t head_id, uint64_t capacity_alloc) {
    if (head_id >= MAX_HEADS) return false;

    heads[head_id].active = true;
    heads[head_id].allocated_capacity = capacity_alloc;
    heads[head_id].used_capacity = 0;

    rebalance_bandwidth();
    return true;
}

void MHSLDDevice::deactivate_head(uint32_t head_id) {
    if (head_id >= MAX_HEADS) return;

    heads[head_id].active = false;
    heads[head_id].bandwidth_share = 0.0;

    // Invalidate all cachelines owned by this head
    std::shared_lock<std::shared_mutex> lock(directory_mutex);
    for (auto& [addr, info] : directory) {
        std::lock_guard<std::mutex> cl_lock(info->lock);
        if (info->owner_head == head_id) {
            if (info->owner_state == MHSLDCacheState::MODIFIED) {
                info->has_dirty_data = true;
            }
            info->owner_head = UINT32_MAX;
            info->owner_state = MHSLDCacheState::INVALID;
        }
        info->sharers.erase(head_id);
    }

    rebalance_bandwidth();
}

void MHSLDDevice::rebalance_bandwidth() {
    uint32_t active_count = 0;
    for (const auto& h : heads) {
        if (h.active) active_count++;
    }

    if (active_count == 0) return;

    // Weighted fair share based on allocated capacity
    uint64_t total_alloc = 0;
    for (const auto& h : heads) {
        if (h.active) total_alloc += h.allocated_capacity;
    }

    for (auto& h : heads) {
        if (h.active && total_alloc > 0) {
            h.bandwidth_share = static_cast<double>(h.allocated_capacity) / total_alloc;
        } else if (h.active) {
            h.bandwidth_share = 1.0 / active_count;
        } else {
            h.bandwidth_share = 0.0;
        }
    }
}

uint64_t MHSLDDevice::allocate_pool(uint32_t head_id, uint64_t size) {
    if (head_id >= MAX_HEADS || !heads[head_id].active) return 0;

    // Check if head has capacity
    if (heads[head_id].used_capacity + size > heads[head_id].allocated_capacity) {
        return 0; // Out of pool
    }

    // Simple allocation: use head_id as base offset + used_capacity
    uint64_t base = static_cast<uint64_t>(head_id) * (total_capacity / MAX_HEADS);
    uint64_t addr = base + heads[head_id].used_capacity;
    heads[head_id].used_capacity += size;

    return addr;
}

void MHSLDDevice::release_pool(uint32_t head_id, uint64_t addr, uint64_t size) {
    if (head_id >= MAX_HEADS || !heads[head_id].active) return;

    // Invalidate cachelines in the released region
    uint64_t cl_addr = addr & ~(CACHELINE_SIZE - 1);
    uint64_t end_addr = addr + size;

    while (cl_addr < end_addr) {
        auto* entry = get_entry(cl_addr);
        if (entry) {
            std::lock_guard<std::mutex> lock(entry->lock);
            entry->sharers.erase(head_id);
            if (entry->owner_head == head_id) {
                entry->owner_head = UINT32_MAX;
                entry->owner_state = MHSLDCacheState::INVALID;
            }
        }
        cl_addr += CACHELINE_SIZE;
    }

    if (heads[head_id].used_capacity >= size) {
        heads[head_id].used_capacity -= size;
    }
}

double MHSLDDevice::get_pool_utilization() const {
    uint64_t total_used = 0;
    for (const auto& h : heads) {
        if (h.active) total_used += h.used_capacity;
    }
    return total_capacity > 0 ? static_cast<double>(total_used) / total_capacity : 0.0;
}

MHSLDCachelineInfo* MHSLDDevice::get_or_create_entry(uint64_t addr) {
    uint64_t cl_addr = addr & ~(CACHELINE_SIZE - 1);

    {
        std::shared_lock<std::shared_mutex> lock(directory_mutex);
        auto it = directory.find(cl_addr);
        if (it != directory.end()) {
            return it->second.get();
        }
    }

    std::unique_lock<std::shared_mutex> lock(directory_mutex);
    auto& entry = directory[cl_addr];
    if (!entry) {
        entry = std::make_unique<MHSLDCachelineInfo>();
        entry->address = cl_addr;
    }
    return entry.get();
}

MHSLDCachelineInfo* MHSLDDevice::get_entry(uint64_t addr) {
    uint64_t cl_addr = addr & ~(CACHELINE_SIZE - 1);
    std::shared_lock<std::shared_mutex> lock(directory_mutex);
    auto it = directory.find(cl_addr);
    return (it != directory.end()) ? it->second.get() : nullptr;
}

/*
 * Read with coherency protocol using LogP model for latency calculation.
 *
 * State transitions for read:
 *   INVALID -> SHARED (fetch from memory or from owner)
 *   SHARED  -> SHARED (hit, no state change)
 *   EXCLUSIVE -> SHARED (if another head reads, owner downgrades)
 *   MODIFIED -> OWNED+SHARED (owner provides data, keeps dirty copy)
 */
double MHSLDDevice::read_with_coherency(uint32_t head_id, uint64_t addr, uint64_t timestamp) {
    if (head_id >= MAX_HEADS || !heads[head_id].active) return 0.0;

    MHSLDCachelineInfo* entry = get_or_create_entry(addr);
    std::lock_guard<std::mutex> lock(entry->lock);

    double latency = base_read_latency;
    entry->access_count++;
    entry->last_access_time = timestamp;
    heads[head_id].total_reads++;

    // Check if this head already has access
    if (entry->owner_head == head_id &&
        (entry->owner_state == MHSLDCacheState::EXCLUSIVE ||
         entry->owner_state == MHSLDCacheState::MODIFIED ||
         entry->owner_state == MHSLDCacheState::OWNED)) {
        // Local hit - no coherency overhead
        latency += calculate_contention_latency(head_id, timestamp);
        return latency;
    }

    if (entry->sharers.count(head_id) > 0) {
        // Already a sharer - local hit
        latency += calculate_contention_latency(head_id, timestamp);
        return latency;
    }

    // Need to acquire shared access
    switch (entry->owner_state) {
        case MHSLDCacheState::INVALID:
            // First access - fetch from device memory
            entry->sharers.insert(head_id);
            entry->owner_state = MHSLDCacheState::SHARED;
            entry->owner_head = head_id;
            // Memory access latency only
            break;

        case MHSLDCacheState::SHARED:
            // Add as sharer, no invalidation needed
            entry->sharers.insert(head_id);
            break;

        case MHSLDCacheState::EXCLUSIVE:
        case MHSLDCacheState::MODIFIED: {
            // Must downgrade owner to SHARED/OWNED
            // LogP latency for snoop message to owner + response
            double snoop_latency = logp_model.message_latency(timestamp, entry->owner_head);

            if (entry->owner_state == MHSLDCacheState::MODIFIED) {
                // Owner transitions to OWNED (keeps dirty copy, allows shared reads)
                entry->owner_state = MHSLDCacheState::OWNED;
                total_downgrades++;
            } else {
                // EXCLUSIVE -> SHARED
                entry->sharers.insert(entry->owner_head);
                entry->owner_state = MHSLDCacheState::SHARED;
            }
            entry->sharers.insert(head_id);
            heads[head_id].coherency_stalls++;
            total_coherency_messages++;

            latency += snoop_latency;
            break;
        }

        case MHSLDCacheState::OWNED:
            // Owner has dirty copy; just add as sharer
            entry->sharers.insert(head_id);
            // LogP latency for data forward from owner
            latency += logp_model.message_latency(timestamp, entry->owner_head);
            total_coherency_messages++;
            break;
    }

    latency += calculate_contention_latency(head_id, timestamp);
    logp_model.record_message(head_id, static_cast<uint64_t>(latency));
    return latency;
}

/*
 * Write with coherency protocol using LogP model.
 *
 * State transitions for write:
 *   INVALID -> MODIFIED (acquire exclusive, no sharers)
 *   SHARED -> MODIFIED (invalidate all sharers)
 *   EXCLUSIVE -> MODIFIED (already exclusive, just mark dirty)
 *   MODIFIED -> MODIFIED (hit if same head, else invalidate+acquire)
 *   OWNED -> MODIFIED (invalidate sharers, keep ownership)
 */
double MHSLDDevice::write_with_coherency(uint32_t head_id, uint64_t addr, uint64_t timestamp) {
    if (head_id >= MAX_HEADS || !heads[head_id].active) return 0.0;

    MHSLDCachelineInfo* entry = get_or_create_entry(addr);
    std::lock_guard<std::mutex> lock(entry->lock);

    double latency = base_write_latency;
    entry->access_count++;
    entry->last_access_time = timestamp;
    entry->version++;
    heads[head_id].total_writes++;

    // Check if this head already has exclusive/modified access
    if (entry->owner_head == head_id) {
        if (entry->owner_state == MHSLDCacheState::EXCLUSIVE ||
            entry->owner_state == MHSLDCacheState::MODIFIED) {
            // Local hit - just mark as modified
            entry->owner_state = MHSLDCacheState::MODIFIED;
            entry->has_dirty_data = true;
            latency += calculate_contention_latency(head_id, timestamp);
            return latency;
        }
        if (entry->owner_state == MHSLDCacheState::OWNED) {
            // Need to invalidate sharers first
            double inv_latency = invalidate_sharers(addr, head_id, timestamp);
            entry->owner_state = MHSLDCacheState::MODIFIED;
            entry->has_dirty_data = true;
            latency += inv_latency;
            latency += calculate_contention_latency(head_id, timestamp);
            return latency;
        }
    }

    // Need to acquire exclusive access
    double coherency_latency = 0.0;

    switch (entry->owner_state) {
        case MHSLDCacheState::INVALID:
            // First access - no coherency needed
            break;

        case MHSLDCacheState::SHARED:
            // Invalidate all sharers
            coherency_latency = invalidate_sharers(addr, head_id, timestamp);
            break;

        case MHSLDCacheState::EXCLUSIVE:
        case MHSLDCacheState::MODIFIED:
        case MHSLDCacheState::OWNED: {
            // Must invalidate owner and all sharers
            if (entry->owner_head != head_id) {
                // Snoop to current owner
                double snoop_latency = logp_model.message_latency(timestamp, entry->owner_head);
                coherency_latency += snoop_latency;

                if (entry->owner_state == MHSLDCacheState::MODIFIED ||
                    entry->owner_state == MHSLDCacheState::OWNED) {
                    // Owner must writeback before relinquishing
                    total_writebacks++;
                }
                heads[entry->owner_head].back_invalidations++;
                total_coherency_messages++;
            }
            // Invalidate sharers
            coherency_latency += invalidate_sharers(addr, head_id, timestamp);
            break;
        }
    }

    // Transition to MODIFIED for the requesting head
    entry->owner_head = head_id;
    entry->owner_state = MHSLDCacheState::MODIFIED;
    entry->sharers.clear();
    entry->has_dirty_data = true;
    heads[head_id].coherency_stalls += (coherency_latency > 0) ? 1 : 0;

    latency += coherency_latency;
    latency += calculate_contention_latency(head_id, timestamp);
    logp_model.record_message(head_id, static_cast<uint64_t>(latency));
    return latency;
}

/*
 * Atomic operation with coherency.
 * Atomics always require exclusive access with serialization.
 */
double MHSLDDevice::atomic_with_coherency(uint32_t head_id, uint64_t addr, uint64_t timestamp) {
    // Atomic requires exclusive access + serialization penalty
    double latency = write_with_coherency(head_id, addr, timestamp);

    // Additional serialization overhead for atomic operations
    // LogP model: atomic requires round-trip to device
    double atomic_overhead = logp_model.config.o_s + logp_model.config.o_r;
    latency += atomic_overhead;

    return latency;
}

/*
 * Invalidate all sharers of a cacheline except the requesting head.
 * Uses LogP model for parallel invalidation with max latency.
 */
double MHSLDDevice::invalidate_sharers(uint64_t addr, uint32_t except_head, uint64_t timestamp) {
    MHSLDCachelineInfo* entry = get_entry(addr);
    if (!entry) return 0.0;

    double max_inv_latency = 0.0;
    std::set<uint32_t> to_invalidate;

    for (uint32_t sharer : entry->sharers) {
        if (sharer != except_head && heads[sharer].active) {
            to_invalidate.insert(sharer);
        }
    }

    // Parallel invalidations: latency = max of all individual invalidations
    // (LogP allows concurrent sends with gap constraint)
    double accumulated_gap = 0.0;
    for (uint32_t sharer : to_invalidate) {
        // Each invalidation incurs sender gap + network latency + receiver overhead
        double inv_latency = accumulated_gap + logp_model.config.L + logp_model.config.o_r;
        max_inv_latency = std::max(max_inv_latency, inv_latency);
        accumulated_gap += logp_model.config.g; // Gap between consecutive sends

        heads[sharer].back_invalidations++;
        total_invalidations++;
        total_coherency_messages++;
    }

    // Add sender overhead (paid once)
    if (!to_invalidate.empty()) {
        max_inv_latency += logp_model.config.o_s;
    }

    entry->sharers.clear();
    return max_inv_latency;
}

/*
 * Downgrade owner from EXCLUSIVE/MODIFIED to SHARED.
 * Owner retains a copy but transitions to shared state.
 */
double MHSLDDevice::downgrade_owner(uint64_t addr, uint32_t requesting_head, uint64_t timestamp) {
    MHSLDCachelineInfo* entry = get_entry(addr);
    if (!entry || entry->owner_head == UINT32_MAX) return 0.0;

    uint32_t owner = entry->owner_head;
    if (owner == requesting_head) return 0.0;

    // LogP round-trip for downgrade request + acknowledgement
    double down_latency = logp_model.message_latency(timestamp, owner);

    // Owner transitions state
    if (entry->owner_state == MHSLDCacheState::MODIFIED) {
        entry->owner_state = MHSLDCacheState::OWNED;
        entry->has_dirty_data = true;
    } else if (entry->owner_state == MHSLDCacheState::EXCLUSIVE) {
        entry->sharers.insert(owner);
        entry->owner_state = MHSLDCacheState::SHARED;
        entry->owner_head = UINT32_MAX;
    }

    total_downgrades++;
    total_coherency_messages++;
    return down_latency;
}

/*
 * Writeback dirty data from a head.
 */
double MHSLDDevice::writeback(uint64_t addr, uint32_t head_id, uint64_t timestamp) {
    MHSLDCachelineInfo* entry = get_entry(addr);
    if (!entry) return 0.0;

    std::lock_guard<std::mutex> lock(entry->lock);

    if (entry->owner_head != head_id) return 0.0;

    double wb_latency = base_write_latency;

    if (entry->owner_state == MHSLDCacheState::MODIFIED ||
        entry->owner_state == MHSLDCacheState::OWNED) {
        // LogP latency for data transfer back to device
        wb_latency += logp_model.config.o_s + logp_model.config.L;
        entry->has_dirty_data = false;
    }

    entry->owner_state = MHSLDCacheState::INVALID;
    entry->owner_head = UINT32_MAX;
    entry->sharers.erase(head_id);

    total_writebacks++;
    return wb_latency;
}

/*
 * Calculate bandwidth contention latency for a head.
 * When multiple heads contend for bandwidth, each head gets its fair share.
 * Additional latency = (1/fair_share - 1) * base_latency when contended.
 */
double MHSLDDevice::calculate_contention_latency(uint32_t head_id, uint64_t timestamp) const {
    if (head_id >= MAX_HEADS || !heads[head_id].active) return 0.0;

    // Count active heads with recent activity
    uint32_t contending_heads = 0;
    for (const auto& h : heads) {
        if (h.active && (h.total_reads + h.total_writes) > 0) {
            contending_heads++;
        }
    }

    if (contending_heads <= 1) return 0.0;

    // Fair share bandwidth for this head
    double fair_share = heads[head_id].bandwidth_share;
    if (fair_share <= 0.0) fair_share = 1.0 / contending_heads;

    // Contention delay: inversely proportional to fair share
    // When fully contended (N heads), each gets 1/N of bandwidth
    // Additional latency = base_latency * (1/share - 1) * contention_factor
    double contention_factor = 0.3; // How much contention affects latency
    double additional_latency = base_read_latency * (1.0 / fair_share - 1.0) * contention_factor;

    // Cap at reasonable maximum
    return std::min(additional_latency, base_read_latency * 5.0);
}

/*
 * Calculate the fair share bandwidth available to a specific head.
 * Returns bandwidth in GB/s.
 */
double MHSLDDevice::calculate_fair_share_bandwidth(uint32_t head_id) const {
    if (head_id >= MAX_HEADS || !heads[head_id].active) return 0.0;
    return max_bandwidth * heads[head_id].bandwidth_share;
}

MHSLDDevice::Stats MHSLDDevice::get_stats() const {
    Stats stats;
    stats.coherency_messages = total_coherency_messages.load();
    stats.invalidations = total_invalidations.load();
    stats.downgrades = total_downgrades.load();
    stats.writebacks = total_writebacks.load();
    stats.pool_utilization = get_pool_utilization();

    // Calculate average latencies from head stats
    uint64_t total_reads = 0, total_writes = 0;
    for (const auto& h : heads) {
        if (h.active) {
            total_reads += h.total_reads;
            total_writes += h.total_writes;
        }
    }
    stats.avg_read_latency = base_read_latency; // Simplified
    stats.avg_write_latency = base_write_latency;
    return stats;
}

/* ============================================================================
 * FabricLink Implementation
 *
 * Models CXL fabric link with latency, bandwidth, and congestion.
 * ============================================================================ */

FabricLink::FabricLink(uint32_t src, uint32_t dst, const FabricLinkConfig& cfg)
    : config_(cfg), src_node_(src), dst_node_(dst),
      available_credits_(cfg.max_credits) {}

double FabricLink::calculate_traversal_latency(uint64_t timestamp, size_t data_size) {
    std::lock_guard<std::mutex> lock(link_mutex_);

    // Base link latency
    double latency = config_.latency_ns;

    // Serialization delay: time to push data onto the link
    // bandwidth_gbps is in GB/s, data_size is in bytes
    double serialization_ns = static_cast<double>(data_size) /
                              (config_.bandwidth_gbps * 1e9) * 1e9;
    latency += serialization_ns;

    // Congestion delay based on credit availability
    latency += get_congestion_delay(timestamp);

    // Track flit time for utilization calculation
    last_flit_time_ = timestamp;
    in_flight_flits_++;

    return latency;
}

bool FabricLink::acquire_credit() {
    size_t current = available_credits_.load();
    while (current > 0) {
        if (available_credits_.compare_exchange_weak(current, current - 1)) {
            return true;
        }
    }
    return false;
}

void FabricLink::release_credit() {
    size_t current = available_credits_.load();
    if (current < config_.max_credits) {
        available_credits_.fetch_add(1);
    }
}

double FabricLink::get_congestion_delay(uint64_t timestamp) const {
    // Non-linear congestion model based on credit availability
    double utilization = 1.0 - (static_cast<double>(available_credits_.load()) /
                                 config_.max_credits);

    if (utilization < 0.5) {
        return 0.0;  // No congestion
    } else if (utilization < 0.8) {
        // Linear 0-20ns in [0.5, 0.8] range
        return (utilization - 0.5) / 0.3 * 20.0;
    } else {
        // Steep 20-100ns in [0.8, 1.0] range
        return 20.0 + (utilization - 0.8) / 0.2 * 80.0;
    }
}

double FabricLink::get_utilization(uint64_t window_ns) const {
    // Simple utilization estimate based on credits used
    double credit_util = 1.0 - (static_cast<double>(available_credits_.load()) /
                                 config_.max_credits);
    return credit_util;
}

/* ============================================================================
 * RemoteCXLExpander Implementation
 *
 * Virtual endpoint that represents remote node memory in the topology tree.
 * ============================================================================ */

RemoteCXLExpander::RemoteCXLExpander(int id, uint32_t remote_node, uint32_t local_node,
                                     uint64_t remote_base, uint64_t remote_capacity,
                                     const FabricLinkConfig& link_cfg)
    : CXLMemExpander(25, 25, 100, 150, id, static_cast<int>(remote_capacity / (1024*1024))),
      remote_node_id_(remote_node), local_node_id_(local_node),
      remote_base_addr_(remote_base), remote_capacity_(remote_capacity) {
    fabric_link_ = std::make_unique<FabricLink>(local_node, remote_node, link_cfg);
}

double RemoteCXLExpander::calculate_latency(const std::vector<std::tuple<uint64_t, uint64_t>>& elem,
                                             double dramlatency) {
    if (elem.empty()) return 0.0;

    double total_latency = 0.0;
    size_t access_count = 0;

    for (const auto& [timestamp, addr] : elem) {
        // Check if this address belongs to our remote range
        if (addr < remote_base_addr_ || addr >= remote_base_addr_ + remote_capacity_) {
            continue;
        }

        double access_latency = 0.0;

        // Frontend processing
        access_latency += frontend_latency_;  // 10ns

        // Forward path
        access_latency += forward_latency_;   // 15ns

        // Fabric link traversal
        access_latency += fabric_link_->calculate_traversal_latency(timestamp, 64);

        // Coherency engine check (if available)
        if (coherency_engine_) {
            CoherencyRequest req{addr, local_node_id_, 0, false, timestamp};
            auto coh = coherency_engine_->process_read(req);
            access_latency += coh.latency_ns;
        }

        // Remote device access latency
        access_latency += this->latency.read;

        // Response path
        access_latency += response_latency_;  // 20ns

        // Congestion
        access_latency += calculate_congestion_delay(timestamp);

        total_latency += access_latency;
        access_count++;
    }

    return access_count > 0 ? total_latency / access_count : 0.0;
}

double RemoteCXLExpander::calculate_bandwidth(const std::vector<std::tuple<uint64_t, uint64_t>>& elem) {
    if (elem.empty()) return 0.0;

    // Remote bandwidth is limited by fabric link bandwidth
    uint64_t current_time = std::get<0>(elem.back());
    uint64_t window_start = current_time - 20000000; // 20ms window

    size_t access_count = 0;
    for (const auto& [timestamp, addr] : elem) {
        if (timestamp >= window_start &&
            addr >= remote_base_addr_ && addr < remote_base_addr_ + remote_capacity_) {
            access_count++;
        }
    }

    double time_window_seconds = 0.02;
    uint64_t total_data = access_count * 64; // 64B cache lines
    double bw_gbps = (total_data / time_window_seconds) / (1024.0 * 1024.0 * 1024.0);

    // Cap at fabric link bandwidth
    return std::min(bw_gbps - fabric_link_->config_.bandwidth_gbps, 0.0);
}

int RemoteCXLExpander::insert(uint64_t timestamp, uint64_t tid, uint64_t phys_addr,
                              uint64_t virt_addr, int index) {
    if (index != this->id) return 0;

    last_timestamp = std::max(last_timestamp, timestamp);

    if (phys_addr == 0) {
        this->counter.inc_store();
        return 1;
    }

    // Record in local occupation for latency calculation
    bool address_exists = address_cache.find(phys_addr) != address_cache.end();

    if (address_exists) {
        for (auto it = this->occupation.cbegin(); it != this->occupation.cend(); it++) {
            if (it->address == phys_addr) {
                this->occupation.erase(it);
                this->occupation.emplace_back(timestamp, phys_addr, 0);
                this->counter.inc_load();

                // Update shadow directory
                update_shadow(phys_addr, MHSLDCacheState::SHARED, timestamp);
                return 2;
            }
        }
    }

    this->occupation.emplace_back(timestamp, phys_addr, 0);
    address_cache.insert(phys_addr);
    this->counter.inc_store();

    // Update shadow directory for new entry
    update_shadow(phys_addr, MHSLDCacheState::SHARED, timestamp);
    return 1;
}

void RemoteCXLExpander::invalidate_shadow(uint64_t addr) {
    uint64_t cl_addr = addr & ~63ULL;
    std::unique_lock<std::shared_mutex> lock(shadow_mutex_);
    auto it = shadow_directory_.find(cl_addr);
    if (it != shadow_directory_.end()) {
        it->second.valid = false;
        it->second.local_state = MHSLDCacheState::INVALID;
    }
}

void RemoteCXLExpander::update_shadow(uint64_t addr, MHSLDCacheState state, uint64_t ts) {
    uint64_t cl_addr = addr & ~63ULL;
    std::unique_lock<std::shared_mutex> lock(shadow_mutex_);
    shadow_directory_[cl_addr] = {state, ts, true};
}
