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
    // 确定页面大小
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

    // 检查本地内存是否有足够空间
    if (controller->occupation.size() * per_size / 1024 / 1024 < controller->capacity * 0.9) {
        return -1; // 返回-1表示使用本地内存
    }

    // 初始化延迟评分
    if (this->latency_scores.empty()) {
        for (size_t i = 0; i < controller->cur_expanders.size(); i++) {
            // 计算综合延迟分数：考虑读写延迟的加权平均
            double read_weight = 0.7; // 读操作权重
            double write_weight = 0.3; // 写操作权重
            double latency_score = 1.0 / (read_weight * controller->cur_expanders[i]->latency.read +
                                          write_weight * controller->cur_expanders[i]->latency.write);
            latency_scores.push_back(latency_score);
        }
    }

    // 寻找最佳节点
    int best_node = -1;
    double best_score = -1;

    for (size_t i = 0; i < controller->cur_expanders.size(); i++) {
        // 检查节点是否有足够容量
        if (controller->cur_expanders[i]->occupation.size() * per_size / 1024 / 1024 >=
            controller->cur_expanders[i]->capacity) {
            continue; // 跳过已满的节点
        }

        // 计算节点评分
        double current_score =
            latency_scores[i] * (1.0 - static_cast<double>(controller->cur_expanders[i]->occupation.size() * per_size) /
                                           (controller->cur_expanders[i]->capacity * 1024 * 1024));

        // 更新最佳节点
        if (current_score > best_score) {
            best_score = current_score;
            best_node = i;
        }
    }

    // 如果找不到合适的节点，返回第一个未满的节点
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

// HugePagePolicy实现
// 大页面策略实现
int HugePagePolicy::compute_once(CXLController *controller) {
    // 如果当前不是大页面模式,尝试升级到大页面
    if (controller->page_type_ == PAGE) {
        // 检查连续的小页面数量
        size_t consecutive_pages = 0;
        size_t max_consecutive = 0;

        for (size_t i = 1; i < controller->occupation.size(); i++) {
            if (controller->occupation[i].address == controller->occupation[i - 1].address + 4096) {
                consecutive_pages++;
                max_consecutive = std::max(max_consecutive, consecutive_pages);
            } else {
                consecutive_pages = 0;
            }
        }

        // 如果有足够多连续页面,切换到2M大页面
        if (max_consecutive >= 512) { // 512 * 4KB = 2MB
            controller->page_type_ = HUGEPAGE_2M;
            return 1; // 返回1表示进行了页面大小调整
        }
    }
    // 同理检查是否可以升级到1G大页面
    else if (controller->page_type_ == HUGEPAGE_2M) {
        size_t consecutive_pages = 0;
        size_t max_consecutive = 0;

        for (size_t i = 1; i < controller->occupation.size(); i++) {
            if (controller->occupation[i].address == controller->occupation[i - 1].address + 2 * 1024 * 1024) {
                consecutive_pages++;
                max_consecutive = std::max(max_consecutive, consecutive_pages);
            } else {
                consecutive_pages = 0;
            }
        }

        if (max_consecutive >= 512) { // 512 * 2MB = 1GB
            controller->page_type_ = HUGEPAGE_1G;
            return 1;
        }
    }

    return 0; // 返回0表示没有进行调整
}

// FIFOPolicy实现
// 先进先出缓存策略
int FIFOPolicy::compute_once(CXLController *controller) {
    int per_size;
    // 确定页面大小
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

    // 检查缓存是否已满
    if (controller->lru_cache.size() * per_size / 1024 / 1024 >= controller->capacity) {
        // 找到时间戳最小的条目（最早插入的）
        uint64_t oldest_timestamp = UINT64_MAX;
        uint64_t oldest_phys_addr = 0;

        for (const auto& [addr, entry] : controller->lru_cache.cache) {
            if (entry.timestamp < oldest_timestamp) {
                oldest_timestamp = entry.timestamp;
                oldest_phys_addr = addr;
            }
        }

        if (oldest_phys_addr != 0) {
            controller->lru_cache.remove(oldest_phys_addr);
            return 1;  // 返回1表示进行了页面驱逐
        }
    }
    return 0;  // 返回0表示没有进行驱逐
}
bool FrequencyBasedInvalidationPolicy::should_invalidate(uint64_t addr, uint64_t timestamp) {
    // 根据访问频率决定是否应该失效
    auto it = access_count.find(addr);
    if (it != access_count.end()) {
        return it->second < access_threshold;
    }
    return false;
}
std::vector<uint64_t> FrequencyBasedInvalidationPolicy::get_invalidation_list(CXLController* controller){
    std::vector<uint64_t> to_invalidate;

    // 遍历缓存查找低频访问的地址
    for (const auto& [addr, entry] : controller->lru_cache.cache) {
        if (should_invalidate(addr, 0)) {
            to_invalidate.push_back(addr);
        }
    }

    // 清理访问计数（周期性）
    uint64_t current_time = controller->last_timestamp;
    if (current_time - last_cleanup > cleanup_interval) {
        access_count.clear();
        last_cleanup = current_time;
    }

    return to_invalidate;
}
bool FrequencyBasedInvalidationPolicy::should_cache(uint64_t addr, uint64_t timestamp) {
    // 记录访问
    access_count[addr]++;
    return true; // 总是缓存
}
int FrequencyBasedInvalidationPolicy::compute_once(CXLController *controller) {
    // 如果有需要失效的地址，返回正数
    return !get_invalidation_list(controller).empty() ? 1 : 0;
}
