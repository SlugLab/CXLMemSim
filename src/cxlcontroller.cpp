/*
 * CXLMemSim controller
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "cxlcontroller.h"
#include "bpftimeruntime.h"
#include "lbr.h"
#include "monitor.h"

void CXLController::insert_end_point(CXLMemExpander *end_point) { this->cur_expanders.emplace_back(end_point); }

void CXLController::construct_topo(std::string_view newick_tree) {
    auto tokens = tokenize(newick_tree);
    std::vector<CXLSwitch *> stk;
    stk.push_back(this);
    for (const auto &token : tokens) {
        if (token == "(" && num_switches == 0) {
            num_switches++;
        } else if (token == "(") {
            /** if is not on the top level */
            auto cur = new CXLSwitch(num_switches++);
            stk.back()->switches.push_back(cur);
            stk.push_back(cur);
        } else if (token == ")") {
            if (!stk.empty()) {
                stk.pop_back();
            } else {
                throw std::invalid_argument("Unbalanced number of parentheses");
            }
        } else if (token == ",") {
        } else {
            stk.back()->expanders.emplace_back(this->cur_expanders[atoi(token.c_str()) - 1]);
            device_map[num_end_points] = this->cur_expanders[atoi(token.c_str()) - 1];
            num_end_points++;
        }
    }
}

CXLController::CXLController(std::array<Policy *, 4> p, int capacity, page_type page_type_, int epoch,
                             double dramlatency)
    : CXLSwitch(0), capacity(capacity), allocation_policy(dynamic_cast<AllocationPolicy *>(p[0])),
      migration_policy(dynamic_cast<MigrationPolicy *>(p[1])), paging_policy(dynamic_cast<PagingPolicy *>(p[2])),
      caching_policy(dynamic_cast<CachingPolicy *>(p[3])), page_type_(page_type_), dramlatency(dramlatency),
      lru_cache(32 * 1024 * 1024 / 64) {
    for (auto switch_ : this->switches) {
        switch_->set_epoch(epoch);
    }
    for (auto expander : this->expanders) {
        expander->set_epoch(epoch);
    }
}

double CXLController::calculate_latency(const std::vector<std::tuple<uint64_t, uint64_t>> &elem, double dramlatency) {
    return CXLSwitch::calculate_latency(elem, dramlatency);
}

double CXLController::calculate_bandwidth(const std::vector<std::tuple<uint64_t, uint64_t>> &elem) {
    return CXLSwitch::calculate_bandwidth(elem);
}

void CXLController::set_stats(mem_stats stats) {
    // SPDLOG_INFO("stats: {} {} {} {} {}", stats.total_allocated, stats.total_freed, stats.current_usage,
    // stats.allocation_count, stats.free_count);
    if (stats.total_allocated < 100000000000) {
        for (auto switch_ : this->switches) {
            switch_->free_stats((stats.total_freed - this->freed) / num_end_points);
        }
        for (auto expander_ : this->expanders) {
            expander_->free_stats((stats.total_freed - this->freed) / num_end_points);
        }
    }
    if (stats.total_freed > this->freed)
        this->freed = stats.total_freed;
}

void CXLController::set_process_info(const proc_info &process_info) {
    monitors->enable(process_info.current_pid, process_info.current_tid, true, 1000, helper.num_of_cpu());
}

void CXLController::set_thread_info(const proc_info &thread_info) {
    if (thread_info.current_pid == monitors->mon[0].tgid) {
        monitors->enable(thread_info.current_pid, thread_info.current_tid, false, 0, helper.num_of_cpu());
        // std::cout << "set thread info " << thread_info.current_pid << " " << thread_info.current_tid << std::endl;
        auto lbr_ = new lbr{.from = 0, .to = 0, .flags = 0};
        this->insert_one(thread_map[thread_info.current_tid], *lbr_);
        delete lbr_;
    }
}

void CXLController::perform_migration() {
    if (!migration_policy)
        return;

    // 获取需要迁移的列表 <物理地址, 大小>
    auto migration_list = migration_policy->get_migration_list(this);

    // 对每个迁移项执行迁移
    for (const auto &[addr, size] : migration_list) {
        // 查找当前地址所在的设备
        CXLMemExpander *src_expander = nullptr;
        int src_id = -1;

        // 检查当前地址是否在控制器本地
        bool in_controller = false;
        for (const auto &[timestamp, info] : occupation) {
            if (info.address == addr) {
                in_controller = true;
                break;
            }
        }

        // 如果不在控制器中，查找它在哪个扩展器
        if (!in_controller) {
            for (auto expander : this->expanders) {
                for (const auto &info : expander->occupation) {
                    if (info.address == addr) {
                        src_expander = expander;
                        break;
                    }
                }
                if (src_expander)
                    break;
            }

            // 如果还没找到，递归搜索所有交换机
            if (!src_expander) {
                std::function<CXLMemExpander *(CXLSwitch *, uint64_t)> find_in_switch =
                    [&find_in_switch](CXLSwitch *sw, uint64_t addr) -> CXLMemExpander * {
                    if (!sw)
                        return nullptr;

                    // 检查此交换机下的扩展器
                    for (auto expander : sw->expanders) {
                        for (const auto &info : expander->occupation) {
                            if (info.address == addr) {
                                return expander;
                            }
                        }
                    }

                    // 递归检查子交换机
                    for (auto child_sw : sw->switches) {
                        CXLMemExpander *result = find_in_switch(child_sw, addr);
                        if (result)
                            return result;
                    }

                    return nullptr;
                };

                src_expander = find_in_switch(this, addr);
            }
        }

        // 选择目标设备（这里简单地选择控制器）
        // 在实际应用中，你可能需要更复杂的目标选择逻辑
        if (in_controller) {
            // 如果数据已在控制器中，选择一个负载较轻的扩展器
            // 这里简单地选择第一个扩展器
            if (!expanders.empty()) {
                CXLMemExpander *dst_expander = expanders[0];

                // 从控制器迁移到扩展器
                for (auto it = occupation.begin(); it != occupation.end(); ++it) {
                    if (it->second.address == addr) {
                        // 复制数据到目标扩展器
                        occupation_info new_info = it->second;

                        // 添加到目标扩展器
                        dst_expander->occupation.push_back(new_info);

                        // 更新统计信息
                        dst_expander->counter.migrate_in.increment();

                        // 可选：从控制器中移除
                        occupation.erase(it);
                        break;
                    }
                }
            }
        } else if (src_expander) {
            // 从扩展器迁移到控制器
            // 查找地址在扩展器中的数据
            for (size_t i = 0; i < src_expander->occupation.size(); i++) {
                auto &info = src_expander->occupation[i];
                if (info.address == addr) {
                    // 复制数据到控制器
                    uint64_t current_timestamp = last_timestamp;
                    occupation.emplace(current_timestamp, info);

                    // 更新统计信息
                    src_expander->counter.migrate_out.increment();

                    // 可选：从源扩展器中移除
                    src_expander->occupation.erase(src_expander->occupation.begin() + i);
                    break;
                }
            }
        }
    }
}

void CXLController::delete_entry(uint64_t addr, uint64_t length) { CXLSwitch::delete_entry(addr, length); }

void CXLController::insert_one(thread_info &t_info, lbr &lbr) {
    auto &rob = t_info.rob;
    auto llcm_count = (lbr.flags & LBR_DATA_MASK) >> LBR_DATA_SHIFT;
    auto ins_count = (lbr.flags & LBR_INS_MASK) >> LBR_INS_SHIFT;

    // ——在这里插入 ring_buffer，表示我们接收到了一个新的 lbr
    ring_buffer.push(lbr);

    for (int i = 0; i < llcm_count; i++) {
        if (t_info.llcm_type.empty()) {
            // 如果 llcm_type 为空，直接插入 0
            t_info.llcm_type.push(0);
        }
        rob.m_count[t_info.llcm_type.front()]++;
        t_info.llcm_type_rob.push(t_info.llcm_type.front());
        t_info.llcm_type.pop();
    }
    rob.llcm_count += llcm_count;
    rob.ins_count += ins_count;

    while (rob.ins_count > ROB_SIZE) {
        auto old_lbr = ring_buffer.front();
        llcm_count = (old_lbr.flags & LBR_DATA_MASK) >> LBR_DATA_SHIFT;
        ins_count = (old_lbr.flags & LBR_INS_MASK) >> LBR_INS_SHIFT;

        rob.ins_count -= ins_count;
        rob.llcm_count -= llcm_count;
        rob.llcm_base += llcm_count;

        for (int i = 0; i < llcm_count; i++) {
            rob.m_count[t_info.llcm_type_rob.front()]--;
            t_info.llcm_type_rob.pop();
        }
        ring_buffer.pop();
    }
}
int CXLController::insert(uint64_t timestamp, uint64_t tid, uint64_t phys_addr, uint64_t virt_addr, int index) {
    auto &t_info = thread_map[tid];

    // 计算时间步长
    uint64_t time_step = 0;
    if (index > last_index) {
        time_step = (timestamp - last_timestamp) / (index - last_index);
    }
    uint64_t current_timestamp = last_timestamp;

    bool res = true;
    for (int i = last_index; i < index; i++) {
        // 更新当前时间戳
        current_timestamp += time_step;

        // 首先检查LRU缓存
        auto cache_result = access_cache(phys_addr, current_timestamp);

        if (cache_result.has_value()) {
            // 缓存命中
            this->counter.inc_hitm();
            t_info.llcm_type.push(0); // 本地访问类型
            continue;
        }

        // 缓存未命中，决定分配策略
        auto numa_policy = allocation_policy->compute_once(this);

        // 检查是否需要页表遍历，并获取额外延迟
        uint64_t ptw_latency = 0;
        if (paging_policy) {
            // 判断是远程访问还是本地访问
            bool is_remote = numa_policy != -1;
            ptw_latency = paging_policy->check_page_table_walk(virt_addr, phys_addr, is_remote, page_type_);

            // 如果需要页表遍历，增加延迟
            if (ptw_latency > 0) {
                // 这里可以根据需要处理额外延迟
                latency_lat += ptw_latency;
                // 或者在计算总延迟时考虑这个因素
            }
        }

        if (numa_policy == -1) {
            // 本地访问
            this->occupation.emplace(current_timestamp, occupation_info{phys_addr, 1, current_timestamp + ptw_latency});
            this->counter.inc_local();
            t_info.llcm_type.push(0);

            // 更新缓存
            update_cache(phys_addr, phys_addr, current_timestamp);
        } else {
            // 远程访问
            this->counter.inc_remote();
            for (auto switch_ : this->switches) {
                res &= switch_->insert(current_timestamp + ptw_latency, tid, phys_addr, virt_addr, numa_policy);
            }
            for (auto expander_ : this->expanders) {
                res &= expander_->insert(current_timestamp + ptw_latency, tid, phys_addr, virt_addr, numa_policy);
            }
            t_info.llcm_type.push(1); // 远程访问类型

            // 如果缓存策略允许缓存远程访问的数据
            if (caching_policy->should_cache(phys_addr, current_timestamp)) {
                update_cache(phys_addr, phys_addr, current_timestamp);
            }
        }
    }
    static int request_counter = 0;
    request_counter += (index - last_index);
    if (request_counter >= 1000) {
        if (migration_policy && migration_policy->compute_once(this) > 0) {
            perform_migration();
        }
        if (caching_policy && caching_policy->compute_once(this) > 0) {
            perform_back_invalidation();
        }
        request_counter = 0;
    }
    // 更新最后的索引和时间戳
    last_index = index > 0 ? index : last_index;
    last_timestamp = timestamp;
    return res;
}
int CXLController::insert(uint64_t timestamp, uint64_t tid, lbr lbrs[32], cntr counters[32]) {
    // 处理LBR记录
    for (int i = 0; i < 32; i++) {
        if (!lbrs[i].from) {
            break;
        }
        insert_one(thread_map[tid], lbrs[i]);
    }

    auto all_access = get_access(timestamp);
    auto &t_info = thread_map[tid];

    // 对每个endpoint计算延迟并累加
    double total_latency = 0.0;
    std::function<void(CXLSwitch *)> dfs_calculate = [&](CXLSwitch *node) {
        // 处理当前节点的expanders
        for (auto *expander : node->expanders) {
            total_latency += get_endpoint_rob_latency(expander, all_access, t_info, dramlatency);
        }

        // 递归处理子节点
        for (auto *switch_ : node->switches) {
            dfs_calculate(switch_);
        }
    };

    // 从当前controller开始DFS遍历
    dfs_calculate(this);

    latency_lat += std::max(total_latency + std::get<0>(calculate_congestion()), 0.0);
    bandwidth_lat += std::max(calculate_bandwidth(all_access), 0.0);

    return 0;
}
std::vector<std::string> CXLController::tokenize(const std::string_view &s) {
    std::vector<std::string> res;
    std::string tmp;
    for (char c : s) {
        if (c == '(' || c == ')' || c == ':' || c == ',') {
            if (!tmp.empty()) {
                res.emplace_back(std::move(tmp));
            }
            res.emplace_back(1, c);
        } else {
            tmp += c;
        }
    }
    if (!tmp.empty()) {
        res.emplace_back(std::move(tmp));
    }
    return res;
}
std::vector<std::tuple<uint64_t, uint64_t>> CXLController::get_access(uint64_t timestamp) {
    return CXLSwitch::get_access(timestamp);
}
std::tuple<double, std::vector<uint64_t>> CXLController::calculate_congestion() {
    return CXLSwitch::calculate_congestion();
}
void CXLController::set_epoch(int epoch) { CXLSwitch::set_epoch(epoch); }
// 在CXLController类中添加
void CXLController::perform_back_invalidation() {
    if (!caching_policy)
        return;

    auto invalidation_list = caching_policy->get_invalidation_list(this);

    // 对每个地址执行失效
    for (const auto &addr : invalidation_list) {
        // 从本地缓存中移除
        if (lru_cache.remove(addr)) {
            counter.inc_backinv();
        }
        // 从所有内存扩展器的occupation中移除
        invalidate_in_expanders(addr);
    }
}

// 递归地处理所有扩展器中的失效
void CXLController::invalidate_in_expanders(uint64_t addr) {
    // 处理当前控制器直接连接的扩展器
    for (auto expander : expanders) {
        if (expander) {
            // 从expander的occupation中移除指定地址
            for (auto it = expander->occupation.begin(); it != expander->occupation.end();) {
                if (it->address == addr) {
                    it = expander->occupation.erase(it);
                    counter.inc_backinv();
                } else {
                    ++it;
                }
            }
        }
    }

    // 递归处理所有连接的交换机
    for (auto switch_ : switches) {
        invalidate_in_switch(switch_, addr);
    }
}

// 在交换机及其子节点中执行失效
void CXLController::invalidate_in_switch(CXLSwitch *switch_, uint64_t addr) {
    if (!switch_)
        return;

    // 处理此交换机连接的扩展器
    for (auto expander : switch_->expanders) {
        if (expander) {
            // 从expander的occupation中移除指定地址
            for (auto it = expander->occupation.begin(); it != expander->occupation.end();) {
                if (it->address == addr) {
                    it = expander->occupation.erase(it);
                    counter.inc_backinv();
                } else {
                    ++it;
                }
            }
        }
    }

    // 递归处理子交换机
    for (auto child_switch : switch_->switches) {
        invalidate_in_switch(child_switch, addr);
    }
}