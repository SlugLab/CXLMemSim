//
// Created by victoryang00 on 1/14/23.
//

#include "cxlcontroller.h"

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
            continue;
        } else {
            stk.back()->expanders.emplace_back(this->cur_expanders[atoi(token.c_str()) - 1]);
        }
    }
}

CXLController::CXLController(Policy p, int capacity) : CXLSwitch(0), capacity(capacity) { this->policy = p; }

double CXLController::calculate_latency(double weight, struct Elem *elem) {
    double lat = 0.0;
    for (auto switch_ : this->switches) {
        lat += switch_->calculate_latency(weight, elem);
    }
    for (auto expander_ : this->expanders) {
        lat += expander_->calculate_latency(weight, elem);
    }
    return lat;
}

double CXLController::calculate_bandwidth(BandwidthPass elem) {
    double bw = 0.0;
    for (auto switch_ : this->switches) {
        bw += switch_->calculate_bandwidth(elem);
    }
    for (auto expander_ : this->expanders) {
        bw += expander_->calculate_bandwidth(elem);
    }
    return bw;
}

std::string CXLController::output() {
    std::stringstream out;
    if (!this->switches.empty()) {
        out << "(";
        this->switches[0]->output();
        for (size_t i = 1; i < this->switches.size(); ++i) {
            out << ",";
            this->switches[i]->output();
        }
        out << ")";
    } else if (!this->expanders.empty()) {
        out << "(";
        this->expanders[0]->output();
        for (size_t i = 1; i < this->expanders.size(); ++i) {
            out << ",";
            this->expanders[i]->output();
        }
        out << ")";
    } else {
        out << this->id;
    }
    return out.str();
}

void CXLController::delete_entry(uint64_t addr) {
    for (auto switch_ : this->switches) {
        switch_->delete_entry(addr);
    }
    for (auto expander_ : this->expanders) {
        expander_->delete_entry(addr);
    }
}

void CXLController::insert(uint64_t timestamp, uint64_t phys_addr, uint64_t virt_addr) {}

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