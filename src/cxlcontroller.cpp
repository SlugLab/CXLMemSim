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
#include "../include/distributed_server.h"
#include "lbr.h"

#include <cctype>
#include <charconv>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

bool parse_size_token(const std::string &token, size_t &value) {
    if (token.empty()) {
        return false;
    }

    size_t parsed = 0;
    const auto *first = token.data();
    const auto *last = token.data() + token.size();
    auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc() || ptr != last) {
        return false;
    }

    value = parsed;
    return true;
}

void skip_optional_branch_length(const std::vector<std::string> &tokens, size_t &index) {
    if (index + 2 < tokens.size() && tokens[index + 1] == ":") {
        index += 2;
    }
}

} // namespace

void CXLController::insert_end_point(CXLMemExpander *end_point) { this->cur_expanders.emplace_back(end_point); }

void CXLController::construct_topo(std::string_view newick_tree) {
    auto tokens = tokenize(newick_tree);
    std::vector<CXLSwitch *> stk;
    stk.push_back(this);

    size_t next_named_expander = 0;
    size_t topology_endpoints = 0;

    auto add_local_expander = [&](size_t expander_number, const std::string &token) {
        if (stk.empty()) {
            throw std::invalid_argument("Topology endpoint '" + token + "' appears after the root is closed");
        }
        if (expander_number == 0 || expander_number > this->cur_expanders.size()) {
            throw std::invalid_argument("Topology endpoint '" + token +
                                        "' is out of range; valid local endpoints are 1.." +
                                        std::to_string(this->cur_expanders.size()));
        }

        auto *expander = this->cur_expanders[expander_number - 1];
        if (!expander) {
            throw std::invalid_argument("Topology endpoint '" + token + "' resolves to a null expander");
        }

        stk.back()->expanders.emplace_back(expander);
        device_map[num_end_points] = expander;
        num_end_points++;
        topology_endpoints++;
    };

    for (size_t t = 0; t < tokens.size(); t++) {
        const auto &token = tokens[t];
        if (token == "(" && num_switches == 0) {
            num_switches++;
        } else if (token == "(") {
            /** if is not on the top level */
            if (stk.empty()) {
                throw std::invalid_argument("Topology opens a child switch after the root is closed");
            }
            auto cur = new CXLSwitch(num_switches++);
            stk.back()->switches.push_back(cur);
            stk.push_back(cur);
        } else if (token == ")") {
            if (!stk.empty()) {
                stk.pop_back();
            } else {
                throw std::invalid_argument("Unbalanced number of parentheses");
            }
        } else if (token == "," || token == ";") {
        } else if (token == "R" && t + 4 < tokens.size() && tokens[t + 1] == ":" && tokens[t + 3] == ":") {
            // R:node_id:exp_id - creates RemoteCXLExpander
            size_t remote_node_value = 0;
            if (!parse_size_token(tokens[t + 2], remote_node_value)) {
                throw std::invalid_argument("Invalid remote node id in topology token 'R:" + tokens[t + 2] + "'");
            }
            uint32_t remote_node = static_cast<uint32_t>(remote_node_value);
            // Use default link config; can be overridden later
            FabricLinkConfig link_cfg{100.0, 25.0, 32};
            uint64_t default_capacity = 1024ULL * 1024 * 1024; // 1GB default
            uint64_t default_base = remote_node * default_capacity;
            auto *remote = add_remote_endpoint(remote_node, default_base, default_capacity, link_cfg);
            if (!stk.empty() && stk.back() != this) {
                stk.back()->expanders.emplace_back(remote);
            }
            topology_endpoints++;
            t += 4; // Skip R:node_id:exp_id (5 tokens total: R, :, node_id, :, exp_id)
        } else if (token == "CPU") {
            // QEMU sample topology files use CPU:<distance> as a Newick label.
            // It is not a CXL memory endpoint, so ignore it and its branch length.
            skip_optional_branch_length(tokens, t);
        } else if (token == "CXL") {
            // QEMU sample topology files use CXL:<distance> without an explicit
            // local endpoint id. Map each named CXL leaf to the next configured
            // local expander.
            if (next_named_expander >= this->cur_expanders.size()) {
                throw std::invalid_argument("Topology contains more CXL leaves than configured local expanders");
            }
            add_local_expander(next_named_expander + 1, token);
            next_named_expander++;
            skip_optional_branch_length(tokens, t);
        } else {
            size_t expander_number = 0;
            if (!parse_size_token(token, expander_number)) {
                throw std::invalid_argument("Unsupported topology token '" + token +
                                            "'; use 1-based endpoint ids such as '(1);' or QEMU labels like "
                                            "'(CPU:0,(CXL:100));'");
            }

            add_local_expander(expander_number, token);
            skip_optional_branch_length(tokens, t);
        }
    }

    if (topology_endpoints == 0 && !this->cur_expanders.empty()) {
        throw std::invalid_argument("Topology did not reference any CXL memory endpoints");
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

void CXLController::perform_migration() {
    if (!migration_policy)
        return;

    auto migration_list = migration_policy->get_migration_list(this);

    for (const auto &[addr, size] : migration_list) {
        CXLMemExpander *src_expander = nullptr;
        int src_id = -1;

        bool in_controller = false;
        for (const auto &[timestamp, info] : occupation) {
            if (info.address == addr) {
                in_controller = true;
                break;
            }
        }

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

            if (!src_expander) {
                std::function<CXLMemExpander *(CXLSwitch *, uint64_t)> find_in_switch =
                    [&find_in_switch](CXLSwitch *sw, uint64_t addr) -> CXLMemExpander * {
                    if (!sw)
                        return nullptr;

                    for (auto expander : sw->expanders) {
                        for (const auto &info : expander->occupation) {
                            if (info.address == addr) {
                                return expander;
                            }
                        }
                    }

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

        if (in_controller) {

            if (!expanders.empty()) {
                CXLMemExpander *dst_expander = expanders[0];

                for (auto it = occupation.begin(); it != occupation.end(); ++it) {
                    if (it->second.address == addr) {
                        occupation_info new_info = it->second;

                        dst_expander->occupation.push_back(new_info);

                        dst_expander->counter.migrate_in.increment();

                        occupation.erase(it);
                        break;
                    }
                }
            }
        } else if (src_expander) {
            for (size_t i = 0; i < src_expander->occupation.size(); i++) {
                auto &info = src_expander->occupation[i];
                if (info.address == addr) {
                    uint64_t current_timestamp = last_timestamp;
                    occupation.emplace(current_timestamp, info);

                    src_expander->counter.migrate_out.increment();

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

    //  ring_buffer lbr
    ring_buffer.push(lbr);

    for (int i = 0; i < llcm_count; i++) {
        if (t_info.llcm_type.empty()) {
            //  llcm_type  0
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

    uint64_t time_step = 0;
    if (index > last_index) {
        time_step = (timestamp - last_timestamp) / (index - last_index);
    }
    uint64_t current_timestamp = last_timestamp;

    bool res = true;
    for (int i = last_index; i < index; i++) {
        current_timestamp += time_step;

        // LRU
        auto cache_result = access_cache(phys_addr, current_timestamp);

        if (cache_result.has_value()) {
            this->counter.inc_hitm();
            t_info.llcm_type.push(0);
            continue;
        }

        // HDM Decoder routing path (distributed mode)
        if (hdm_decoder_) {
            auto decode_result = hdm_decoder_->decode(phys_addr);

            uint64_t ptw_latency = 0;
            if (paging_policy) {
                bool is_remote = decode_result.is_remote;
                ptw_latency = paging_policy->check_page_table_walk(virt_addr, phys_addr, is_remote, page_type_);
                if (ptw_latency > 0) {
                    latency_lat += ptw_latency;
                }
            }

            if (decode_result.is_remote) {
                // Remote access via RemoteCXLExpander
                auto *remote = get_remote_expander(decode_result.target_id);
                if (remote) {
                    remote->insert(current_timestamp + ptw_latency, tid, phys_addr, virt_addr, remote->id);
                }
                this->counter.inc_remote();
                t_info.llcm_type.push(1);
            } else {
                // Local access routed by HDM decoder
                auto it = device_map.find(decode_result.target_id);
                if (it != device_map.end()) {
                    it->second->insert(current_timestamp + ptw_latency, tid, phys_addr, virt_addr, it->second->id);
                } else {
                    // No specific device, use local occupation
                    this->occupation.emplace(current_timestamp,
                                             occupation_info{phys_addr, 1, current_timestamp + ptw_latency});
                }
                this->counter.inc_local();
                t_info.llcm_type.push(0);
            }

            // Update cache if allowed
            if (caching_policy && caching_policy->should_cache(phys_addr, current_timestamp)) {
                update_cache(phys_addr, phys_addr, current_timestamp);
            }
        } else {
            // Fallback: old allocation_policy path (backwards compat)
            auto numa_policy = allocation_policy->compute_once(this);

            uint64_t ptw_latency = 0;
            if (paging_policy) {
                bool is_remote = numa_policy != -1;
                ptw_latency = paging_policy->check_page_table_walk(virt_addr, phys_addr, is_remote, page_type_);
                if (ptw_latency > 0) {
                    latency_lat += ptw_latency;
                }
            }

            if (numa_policy == -1) {
                this->occupation.emplace(current_timestamp,
                                         occupation_info{phys_addr, 1, current_timestamp + ptw_latency});
                this->counter.inc_local();
                t_info.llcm_type.push(0);
                update_cache(phys_addr, phys_addr, current_timestamp);
            } else {
                this->counter.inc_remote();
                for (auto switch_ : this->switches) {
                    res &= switch_->insert(current_timestamp + ptw_latency, tid, phys_addr, virt_addr, numa_policy);
                }
                for (auto expander_ : this->expanders) {
                    res &= expander_->insert(current_timestamp + ptw_latency, tid, phys_addr, virt_addr, numa_policy);
                }
                t_info.llcm_type.push(1);

                if (caching_policy->should_cache(phys_addr, current_timestamp)) {
                    update_cache(phys_addr, phys_addr, current_timestamp);
                }
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
    last_index = index > 0 ? index : last_index;
    last_timestamp = timestamp;
    return res;
}

void CXLController::record_cxl_access(uint64_t timestamp, uint64_t tid, uint64_t addr, bool is_write) {
    this->counter.inc_remote();

    int endpoint_id = 0;
    if (!this->cur_expanders.empty() && this->cur_expanders.front() != nullptr) {
        endpoint_id = this->cur_expanders.front()->id;
    }

    int ret = CXLSwitch::record_access(timestamp, tid, addr, addr, endpoint_id, is_write);
    if (ret == 0) {
        for (auto *expander : this->cur_expanders) {
            if (expander == nullptr || expander->id != endpoint_id) {
                continue;
            }
            expander->record_access(timestamp, tid, addr, addr, endpoint_id, is_write);
            break;
        }
    }
}

int CXLController::insert(uint64_t timestamp, uint64_t tid, lbr lbrs[32], cntr counters[32]) {
    // LBR
    for (int i = 0; i < 32; i++) {
        if (!lbrs[i].from) {
            break;
        }
        insert_one(thread_map[tid], lbrs[i]);
    }

    auto all_access = get_access(timestamp);
    auto &t_info = thread_map[tid];

    // endpoint
    double total_latency = 0.0;
    std::function<void(CXLSwitch *)> dfs_calculate = [&](CXLSwitch *node) {
        // expanders
        for (auto *expander : node->expanders) {
            total_latency += get_endpoint_rob_latency(expander, all_access, t_info, dramlatency);
        }

        for (auto *switch_ : node->switches) {
            dfs_calculate(switch_);
        }
    };

    // controllerDFS
    dfs_calculate(this);

    latency_lat += std::max(total_latency + std::get<0>(calculate_congestion()), 0.0);
    bandwidth_lat += std::max(calculate_bandwidth(all_access), 0.0);

    return 0;
}

std::vector<std::string> CXLController::tokenize(const std::string_view &s) {
    std::vector<std::string> res;
    std::string tmp;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!tmp.empty()) {
                res.emplace_back(std::move(tmp));
                tmp.clear();
            }
        } else if (c == '(' || c == ')' || c == ':' || c == ',' || c == ';') {
            if (!tmp.empty()) {
                res.emplace_back(std::move(tmp));
                tmp.clear();
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
// CXLController
void CXLController::perform_back_invalidation() {
    if (!caching_policy)
        return;

    auto invalidation_list = caching_policy->get_invalidation_list(this);

    for (const auto &addr : invalidation_list) {
        if (lru_cache.remove(addr)) {
            counter.inc_backinv();
        }
        // occupation
        invalidate_in_expanders(addr);
    }
}

void CXLController::invalidate_in_expanders(uint64_t addr) {
    for (auto expander : expanders) {
        if (expander) {
            // expanderoccupation
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

    for (auto switch_ : switches) {
        invalidate_in_switch(switch_, addr);
    }
}

void CXLController::invalidate_in_switch(CXLSwitch *switch_, uint64_t addr) {
    if (!switch_)
        return;

    for (auto expander : switch_->expanders) {
        if (expander) {
            // expanderoccupation
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

    for (auto child_switch : switch_->switches) {
        invalidate_in_switch(child_switch, addr);
    }
}

/* ============================================================================
 * LogP Queuing Model Integration
 *
 * Integrates the LogP model into the controller's latency calculation
 * for distributed/multi-node CXL topologies. The LogP parameters model
 * the inter-node communication cost which is added to the base CXL
 * device access latency.
 * ============================================================================ */

void CXLController::configure_logp(const LogPConfig &config) {
    logp_model.reconfigure(config);
    SPDLOG_INFO("CXLController LogP configured: L={:.1f}ns o_s={:.1f}ns o_r={:.1f}ns g={:.1f}ns P={}", config.L,
                config.o_s, config.o_r, config.g, config.P);
}

void CXLController::calibrate_logp_from_tcp(const struct TCPCalibrationResult &result) {
    if (!result.valid) {
        SPDLOG_WARN("Invalid TCP calibration result, keeping existing LogP config");
        return;
    }

    // Clamp values to realistic ranges for TCP
    double L = std::max(0.5, std::min(result.L, 500.0));
    // 0.5-500us latency
    double o_s = std::max(0.1, std::min(result.o_s, 100.0));
    // 0.1-100us send overhead
    double o_r = std::max(0.1, std::min(result.o_r, 100.0));
    // 0.1-100us recv overhead
    double g = std::max(0.01, std::min(result.g, 50.0));
    // 0.01-50us gap

    // Convert from us to ns for LogP config (calibration measures in us)
    LogPConfig calibrated_config(L * 1000.0, // L in ns
                                 o_s * 1000.0, // o_s in ns
                                 o_r * 1000.0, // o_r in ns
                                 g * 1000.0, // g in ns
                                 logp_model.config.P // Keep existing P (number of nodes)
    );

    logp_model.reconfigure(calibrated_config);
    SPDLOG_INFO("CXLController LogP calibrated from TCP ({} samples): "
                "L={:.1f}ns o_s={:.1f}ns o_r={:.1f}ns g={:.1f}ns",
                result.samples, calibrated_config.L, calibrated_config.o_s, calibrated_config.o_r, calibrated_config.g);
}

double CXLController::calculate_logp_latency(uint32_t src_node, uint32_t dst_node, uint64_t timestamp) {
    if (src_node == dst_node)
        return 0.0;
    return logp_model.message_latency(timestamp, dst_node);
}

double CXLController::calculate_logp_broadcast_latency() { return logp_model.broadcast_latency(); }

/* ============================================================================
 * MH-SLD (Multi-Headed Single Logical Device) Integration
 *
 * Enables MH-SLD mode on the controller. In this mode, the controller's
 * expanders are treated as a shared device with multiple heads (one per
 * host/node). Cacheline-state-aware coherency is used for all accesses.
 *
 * The MH-SLD model leverages:
 *   - Per-cacheline MOESI states for tracking cross-head sharing
 *   - LogP-based latency for coherency messages between heads
 *   - Fair-share bandwidth allocation across active heads
 *   - Memory pooling with per-head capacity quotas
 * ============================================================================ */

void CXLController::enable_mhsld(uint32_t num_heads, double bandwidth_gbps) {
    // Compute total capacity across all expanders
    uint64_t total_bytes = 0;
    for (auto *exp : cur_expanders) {
        total_bytes += static_cast<uint64_t>(exp->capacity) * 1024ULL * 1024ULL;
    }

    // Use average read/write latency from first expander as base
    double read_lat = 100.0, write_lat = 150.0;
    if (!cur_expanders.empty()) {
        read_lat = cur_expanders[0]->latency.read;
        write_lat = cur_expanders[0]->latency.write;
    }

    mhsld_device =
        std::make_unique<MHSLDDevice>(total_bytes, num_heads, read_lat, write_lat, bandwidth_gbps, logp_model.config);

    SPDLOG_INFO("MH-SLD enabled on controller: {} heads, {} MB total, {:.1f} GB/s BW", num_heads,
                total_bytes / (1024 * 1024), bandwidth_gbps);
}

double CXLController::mhsld_read(uint32_t head_id, uint64_t addr, uint64_t timestamp) {
    if (!mhsld_device)
        return 0.0;
    return mhsld_device->read_with_coherency(head_id, addr, timestamp);
}

double CXLController::mhsld_write(uint32_t head_id, uint64_t addr, uint64_t timestamp) {
    if (!mhsld_device)
        return 0.0;
    return mhsld_device->write_with_coherency(head_id, addr, timestamp);
}

double CXLController::mhsld_atomic(uint32_t head_id, uint64_t addr, uint64_t timestamp) {
    if (!mhsld_device)
        return 0.0;
    return mhsld_device->atomic_with_coherency(head_id, addr, timestamp);
}

MHSLDDevice::Stats CXLController::get_mhsld_stats() const {
    if (!mhsld_device) {
        return {0, 0, 0, 0, 0.0, 0.0, 0.0};
    }
    return mhsld_device->get_stats();
}

/* ============================================================================
 * DCD / GFAM Integration
 *
 * DCD models runtime tagged capacity and GFAM layers per-host access control
 * and fabric-sharing latency on top of that allocated capacity.
 * ============================================================================ */

void CXLController::enable_dcd(uint64_t total_capacity_bytes, uint64_t granularity_bytes,
                               uint64_t initial_capacity_bytes, uint64_t initial_tag) {
    dcd_device = std::make_unique<DynamicCapacityDevice>(total_capacity_bytes, granularity_bytes);

    if (initial_capacity_bytes > 0) {
        uint64_t initial = std::min(initial_capacity_bytes, total_capacity_bytes);
        auto result = dcd_device->add_capacity(0, initial, initial_tag);
        if (result.status != DCDStatus::OK) {
            SPDLOG_WARN("DCD initial capacity add failed: status={} size={} bytes", static_cast<int>(result.status),
                        initial);
        }
    }

    auto stats = dcd_device->stats();
    SPDLOG_INFO("DCD enabled: total={} MB allocated={} MB granularity={} bytes extents={}",
                stats.total_capacity / (1024 * 1024), stats.allocated_capacity / (1024 * 1024),
                dcd_device->granularity(), stats.active_extents);
}

DCDAllocationResult CXLController::dcd_add_capacity(uint64_t requested_base, uint64_t size, uint64_t tag,
                                                    uint64_t timestamp) {
    if (!dcd_device) {
        return {DCDStatus::INVALID_REQUEST, 0, 0, tag};
    }
    return dcd_device->add_capacity(requested_base, size, tag, timestamp);
}

DCDStatus CXLController::dcd_release_capacity(uint64_t base, uint64_t size, uint64_t tag, uint64_t timestamp) {
    if (!dcd_device) {
        return DCDStatus::INVALID_REQUEST;
    }
    return dcd_device->release_capacity(base, size, tag, timestamp);
}

bool CXLController::dcd_is_allocated(uint64_t addr, uint64_t size) const {
    return !dcd_device || dcd_device->is_allocated(addr, size);
}

DCDStats CXLController::get_dcd_stats() const {
    if (!dcd_device) {
        return {};
    }
    return dcd_device->stats();
}

void CXLController::enable_gfam(uint32_t num_hosts, double fabric_latency_ns, double bandwidth_gbps) {
    if (!dcd_device) {
        uint64_t total_bytes = 0;
        for (auto *exp : cur_expanders) {
            total_bytes += static_cast<uint64_t>(exp->capacity) * 1024ULL * 1024ULL;
        }
        if (total_bytes == 0) {
            total_bytes = static_cast<uint64_t>(capacity) * 1024ULL * 1024ULL;
        }
        enable_dcd(total_bytes, 1024ULL * 1024ULL, total_bytes, 1);
    }

    gfam_device = std::make_unique<GFAMDevice>(dcd_device.get(), fabric_latency_ns, bandwidth_gbps);
    for (uint32_t host = 0; host < num_hosts; host++) {
        gfam_device->register_host(host, "host" + std::to_string(host));
    }

    for (const auto &extent : dcd_device->active_extents()) {
        for (uint32_t host = 0; host < num_hosts; host++) {
            gfam_device->grant_access(host, extent.base, extent.size, DCD_PERM_ALL);
        }
    }

    SPDLOG_INFO("GFAM enabled: hosts={} fabric_latency={:.1f}ns bandwidth={:.1f}GB/s", num_hosts, fabric_latency_ns,
                bandwidth_gbps);
}

DCDStatus CXLController::gfam_grant_access(uint32_t host_id, uint64_t base, uint64_t size, uint32_t permissions) {
    if (!gfam_device) {
        return DCDStatus::INVALID_REQUEST;
    }
    return gfam_device->grant_access(host_id, base, size, permissions);
}

DCDStatus CXLController::gfam_revoke_access(uint32_t host_id, uint64_t base, uint64_t size) {
    if (!gfam_device) {
        return DCDStatus::INVALID_REQUEST;
    }
    return gfam_device->revoke_access(host_id, base, size);
}

GFAMAccessResult CXLController::gfam_record_access(uint32_t host_id, uint64_t addr, uint64_t size, bool is_write,
                                                   bool is_atomic, uint64_t timestamp) {
    if (!gfam_device) {
        return {true, DCDStatus::OK, 0.0};
    }
    return gfam_device->record_access(host_id, addr, size, is_write, is_atomic, timestamp);
}

GFAMStats CXLController::get_gfam_stats() const {
    if (!gfam_device) {
        return {};
    }
    return gfam_device->stats();
}

/* ============================================================================
 * Distributed Topology Configuration
 *
 * Configures the controller for distributed multi-node operation with
 * HDM decoder for address routing and unified CoherencyEngine.
 * ============================================================================ */

void CXLController::configure_distributed(uint32_t local_node_id, HDMDecoderMode mode) {
    local_node_id_ = local_node_id;
    hdm_decoder_ = std::make_unique<HDMDecoder>(mode);
    coherency_ = std::make_unique<CoherencyEngine>(local_node_id, hdm_decoder_.get(), &logp_model);

    SPDLOG_INFO("CXLController configured for distributed mode: node={}, mode={}", local_node_id,
                static_cast<int>(mode));
}

RemoteCXLExpander *CXLController::add_remote_endpoint(uint32_t remote_node, uint64_t base, uint64_t capacity,
                                                      const FabricLinkConfig &link_cfg) {
    int remote_id = num_end_points++;

    auto *remote = new RemoteCXLExpander(remote_id, remote_node, local_node_id_, base, capacity, link_cfg);
    remote->coherency_engine_ = coherency_.get();
    remote->hdm_decoder_ = hdm_decoder_.get();

    // Add to topology
    this->expanders.push_back(remote);
    device_map[remote_id] = remote;
    remote_expanders_.push_back(remote);

    SPDLOG_INFO("Added remote endpoint: node={}, base=0x{:x}, capacity={}MB, id={}", remote_node, base,
                capacity / (1024 * 1024), remote_id);

    return remote;
}

RemoteCXLExpander *CXLController::get_remote_expander(uint32_t node_id) {
    for (auto *remote : remote_expanders_) {
        if (remote->remote_node_id_ == node_id) {
            return remote;
        }
    }
    return nullptr;
}

/*
 * Calculate distributed latency combining:
 *   1. Base CXL device access latency (from endpoint tree traversal)
 *   2. LogP network latency (if remote node access)
 *   3. MH-SLD coherency overhead (if multi-headed sharing active)
 *
 * This is the main entry point for latency calculation in distributed mode.
 */
double CXLController::calculate_distributed_latency(const std::vector<std::tuple<uint64_t, uint64_t>> &elem,
                                                    uint32_t head_id, uint32_t target_node) {

    if (elem.empty())
        return 0.0;

    // 1. Base CXL device latency from tree traversal
    double base_latency = calculate_latency(elem, dramlatency);

    // 2. LogP network latency for remote access
    uint64_t timestamp = std::get<0>(elem.back());
    double network_latency = 0.0;
    if (target_node != head_id) {
        network_latency = calculate_logp_latency(head_id, target_node, timestamp);
        // Update LogP model statistics
        logp_model.record_message(target_node, static_cast<uint64_t>(network_latency));
    }

    // 3. MH-SLD coherency overhead
    double coherency_latency = 0.0;
    if (mhsld_device) {
        // Process each access through MH-SLD coherency
        double total_mhsld = 0.0;
        size_t count = 0;
        for (const auto &[ts, addr] : elem) {
            total_mhsld += mhsld_device->read_with_coherency(head_id, addr, ts);
            count++;
        }
        // Coherency overhead is the difference from base read latency
        if (count > 0) {
            double avg_mhsld = total_mhsld / count;
            coherency_latency = std::max(0.0, avg_mhsld - mhsld_device->base_read_latency);
        }
    }

    return base_latency + network_latency + coherency_latency;
}
