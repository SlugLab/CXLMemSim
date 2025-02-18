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
            num_end_points++;
        }
    }
}

CXLController::CXLController(AllocationPolicy *p, int capacity, page_type page_type_, int epoch)
    : CXLSwitch(0), capacity(capacity), policy(p), page_type_(page_type_) {
    for (auto switch_ : this->switches) {
        switch_->set_epoch(epoch);
    }
    for (auto expander : this->expanders) {
        expander->set_epoch(epoch);
    }
    // TODO get LRU wb
    // TODO BW type series
    // TODO cache
    // TODO back invalidation
    // deferentiate R/W for multi reader multi writer
}

double CXLController::calculate_latency(LatencyPass elem) { return CXLSwitch::calculate_latency(elem); }

double CXLController::calculate_bandwidth(BandwidthPass elem) { return CXLSwitch::calculate_bandwidth(elem); }

std::string CXLController::output() {
    std::string res;
    if (!this->switches.empty()) {
        res += "(";
        res += this->switches[0]->output();
        for (size_t i = 1; i < this->switches.size(); ++i) {
            res += ",";
            res += this->switches[i]->output();
        }
        res += ")";
    }
    if (!this->expanders.empty()) {
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

void CXLController::set_process_info(proc_info process_info) {
    monitors->enable(process_info.current_pid, process_info.current_tid, true, 1000, 0);
}

void CXLController::set_thread_info(proc_info thread_info) {
    monitors->enable(thread_info.current_pid, thread_info.current_tid, false, 0, 0);
}

void CXLController::delete_entry(uint64_t addr, uint64_t length) { CXLSwitch::delete_entry(addr, length); }

int CXLController::insert(uint64_t timestamp, uint64_t tid, lbr lbrs[4], cntr counters[4]) {
    for (auto expander : this->expanders) {
        auto res = expander->insert(timestamp, tid, lbrs, counters);
        if (res != 0) {
            return res;
        }
    }
    return 0;
}
int CXLController::insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr, int index) {
    auto index_ = policy->compute_once(this);
    if (index_ == -1) {
        this->occupation.emplace(timestamp, phys_addr);
        this->va_pa_map.emplace(virt_addr, phys_addr);
        this->counter.inc_local();
        return true;
    }
    this->counter.inc_remote();
    for (auto switch_ : this->switches) {
        auto res = switch_->insert(timestamp, phys_addr, virt_addr, index_);
        if (res != 0) {
            return res;
        };
    }
    for (auto expander_ : this->expanders) {
        auto res = expander_->insert(timestamp, phys_addr, virt_addr, index_);
        if (res != 0) {
            return res;
        };
    }
    return false;
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
std::tuple<int, int> CXLController::get_all_access() { return CXLSwitch::get_all_access(); }
std::tuple<double, std::vector<uint64_t>> CXLController::calculate_congestion() {
    return CXLSwitch::calculate_congestion();
}
void CXLController::set_epoch(int epoch) { CXLSwitch::set_epoch(epoch); }
MigrationPolicy::MigrationPolicy() {}
PagingPolicy::PagingPolicy() {}
CachingPolicy::CachingPolicy() {}