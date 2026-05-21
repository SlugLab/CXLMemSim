/*
 * CXLMemSim policy
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *      Brian Zhao
 *  SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 *  Copyright 2025 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include "policy.h"
#include <numeric>
PagingPolicy::PagingPolicy() = default;
CachingPolicy::CachingPolicy() = default;
AllocationPolicy::AllocationPolicy() = default;
// If the number is -1 for local, else it is the index of the remote server
int InterleavePolicy::compute_once(CXLController *controller) {
    int per_size;
    switch (controller->page_type_) {
    case CACHELINE:
        per_size = 64;
        break;
    case PAGE:
        per_size = 4096;
        break;
    case HUGEPAGE_2M:
        per_size = 2 * 1024 * 1024;
        break;
    case HUGEPAGE_1G:
        per_size = 1024 * 1024 * 1024;
        break;
    };
    if (controller->occupation.size() * per_size / 1024 / 1024 < controller->capacity * 0.9) {
        return -1;
    } else {
        if (this->percentage.empty()) {
            // Here to compute the distributor statically using geometry average of write latency
            std::vector<double> to_store;
            for (auto &i : controller->cur_expanders) {
                to_store.push_back(1 / i->latency.write);
            }
            for (auto &i : to_store) {
                this->percentage.push_back(int(i / std::accumulate(to_store.begin(), to_store.end(), 0.0) * 10));
            }
            this->all_size = std::accumulate(this->percentage.begin(), this->percentage.end(), 0);
        }
    next:
        if (all_size == 0) {
            return -1;
        }
        last_remote = (last_remote + 1) % all_size;
        int sum, index;
        for (index = 0, sum = 0; sum <= last_remote; index++) { // 5 2 2 to get the next
            sum += this->percentage[index];
            if (sum > last_remote) {
                if (controller->cur_expanders[index]->occupation.size() * per_size / 1024 / 1024 <
                    controller->cur_expanders[index]->capacity) {
                    break;
                } else {
                    /** TODO: capacity bound */
                    goto next;
                }
            }
        }
        return index;
    }
}
int NUMAPolicy::compute_once(CXLController *controller) {
    int per_size;
    switch (controller->page_type_) {
    case CACHELINE:
        per_size = 64;
        break;
    case PAGE:
        per_size = 4096;
        break;
    case HUGEPAGE_2M:
        per_size = 2 * 1024 * 1024;
        break;
    case HUGEPAGE_1G:
        per_size = 1024 * 1024 * 1024;
        break;
    };

    if (controller->occupation.size() * per_size / 1024 / 1024 < controller->capacity * 0.9) {
        return -1; // -1
    }

    if (this->latency_scores.empty()) {
        for (size_t i = 0; i < controller->cur_expanders.size(); i++) {
            double read_weight = 0.7;
            double write_weight = 0.3;
            double latency_score = 1.0 / (read_weight * controller->cur_expanders[i]->latency.read +
                                          write_weight * controller->cur_expanders[i]->latency.write);
            latency_scores.push_back(latency_score);
        }
    }

    int best_node = -1;
    double best_score = -1;

    for (size_t i = 0; i < controller->cur_expanders.size(); i++) {
        if (controller->cur_expanders[i]->occupation.size() * per_size / 1024 / 1024 >=
            controller->cur_expanders[i]->capacity) {
            continue;
        }

        double current_score =
            latency_scores[i] * (1.0 - static_cast<double>(controller->cur_expanders[i]->occupation.size() * per_size) /
                                           (controller->cur_expanders[i]->capacity * 1024 * 1024));

        if (current_score > best_score) {
            best_score = current_score;
            best_node = i;
        }
    }

    if (best_node == -1) {
        for (size_t i = 0; i < controller->cur_expanders.size(); i++) {
            if (controller->cur_expanders[i]->occupation.size() * per_size / 1024 / 1024 <
                controller->cur_expanders[i]->capacity) {
                return i;
            }
        }
    }

    return best_node;
}

// FIFOPolicy
int FIFOPolicy::compute_once(CXLController *controller) {
    int per_size;
    switch (controller->page_type_) {
    case CACHELINE:
        per_size = 64;
        break;
    case PAGE:
        per_size = 4096;
        break;
    case HUGEPAGE_2M:
        per_size = 2 * 1024 * 1024;
        break;
    case HUGEPAGE_1G:
        per_size = 1024 * 1024 * 1024;
        break;
    };

    if (controller->lru_cache.size() * per_size / 1024 / 1024 >= controller->capacity) {

        uint64_t oldest_timestamp = UINT64_MAX;
        uint64_t oldest_phys_addr = 0;

        for (const auto &[addr, entry] : controller->lru_cache.cache) {
            if (entry.timestamp < oldest_timestamp) {
                oldest_timestamp = entry.timestamp;
                oldest_phys_addr = addr;
            }
        }

        if (oldest_phys_addr != 0) {
            controller->lru_cache.remove(oldest_phys_addr);
            return 1; // 1
        }
    }
    return 0; // 0
}
bool FrequencyBasedInvalidationPolicy::should_invalidate(uint64_t addr, uint64_t timestamp) {
    auto it = access_count.find(addr);
    if (it != access_count.end()) {
        return it->second < access_threshold;
    }
    return false;
}
std::vector<uint64_t> FrequencyBasedInvalidationPolicy::get_invalidation_list(CXLController *controller) {
    std::vector<uint64_t> to_invalidate;

    for (const auto &[addr, entry] : controller->lru_cache.cache) {
        if (should_invalidate(addr, 0)) {
            to_invalidate.push_back(addr);
        }
    }

    uint64_t current_time = controller->last_timestamp;
    if (current_time - last_cleanup > cleanup_interval) {
        access_count.clear();
        last_cleanup = current_time;
    }

    return to_invalidate;
}
bool FrequencyBasedInvalidationPolicy::should_cache(uint64_t addr, uint64_t timestamp) {
    access_count[addr]++;
    return true;
}
int FrequencyBasedInvalidationPolicy::compute_once(CXLController *controller) {

    return !get_invalidation_list(controller).empty() ? 1 : 0;
}
