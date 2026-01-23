/*
 * CXLMemSim HDM Decoder Implementation
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 * Copyright 2025 Regents of the University of California
 * UC Santa Cruz Sluglab.
 */

#include "hdm_decoder.h"

HDMDecoder::HDMDecoder(HDMDecoderMode mode) : mode_(mode) {}

void HDMDecoder::add_range(uint64_t base, uint64_t size, uint32_t target_id, bool is_remote) {
    ranges_.push_back({base, size, target_id, is_remote});
    ranges_sorted_ = false;
}

void HDMDecoder::configure_interleave(InterleaveGranularity gran,
                                       const std::vector<uint32_t>& targets,
                                       uint64_t base, uint64_t total_size) {
    interleave_config_.granularity = gran;
    interleave_config_.target_ids = targets;
    interleave_config_.base_addr = base;
    interleave_config_.total_size = total_size;
}

void HDMDecoder::sort_ranges() {
    std::sort(ranges_.begin(), ranges_.end(),
              [](const HDMRange& a, const HDMRange& b) {
                  return a.base_addr < b.base_addr;
              });
    ranges_sorted_ = true;
}

HDMDecoder::DecodeResult HDMDecoder::decode(uint64_t addr) const {
    switch (mode_) {
        case HDMDecoderMode::RANGE_BASED:
            return decode_range(addr);
        case HDMDecoderMode::INTERLEAVED:
            return decode_interleaved(addr);
        case HDMDecoderMode::HYBRID: {
            // Try range-based first, fall back to interleaved
            auto result = decode_range(addr);
            if (result.target_id != UINT32_MAX) {
                return result;
            }
            return decode_interleaved(addr);
        }
    }
    return {UINT32_MAX, 0, false, 0};
}

HDMDecoder::DecodeResult HDMDecoder::decode_range(uint64_t addr) const {
    // Binary search through sorted ranges
    if (ranges_.empty()) {
        return {UINT32_MAX, 0, false, 0};
    }

    // If not sorted, do linear scan (const method can't sort)
    if (!ranges_sorted_) {
        for (const auto& range : ranges_) {
            if (addr >= range.base_addr && addr < range.base_addr + range.size) {
                uint64_t offset = addr - range.base_addr;
                uint32_t hops = range.is_remote ? 1 : 0;
                return {range.target_id, offset, range.is_remote, hops};
            }
        }
        return {UINT32_MAX, 0, false, 0};
    }

    // Binary search for the range containing addr
    int lo = 0, hi = static_cast<int>(ranges_.size()) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const auto& range = ranges_[mid];
        if (addr < range.base_addr) {
            hi = mid - 1;
        } else if (addr >= range.base_addr + range.size) {
            lo = mid + 1;
        } else {
            uint64_t offset = addr - range.base_addr;
            uint32_t hops = range.is_remote ? 1 : 0;
            return {range.target_id, offset, range.is_remote, hops};
        }
    }

    return {UINT32_MAX, 0, false, 0};
}

HDMDecoder::DecodeResult HDMDecoder::decode_interleaved(uint64_t addr) const {
    if (interleave_config_.target_ids.empty()) {
        return {UINT32_MAX, 0, false, 0};
    }

    // Check if address is within the interleave region
    if (addr < interleave_config_.base_addr ||
        addr >= interleave_config_.base_addr + interleave_config_.total_size) {
        return {UINT32_MAX, 0, false, 0};
    }

    uint64_t granularity = static_cast<uint64_t>(interleave_config_.granularity);
    uint64_t relative_addr = addr - interleave_config_.base_addr;
    uint64_t block_index = relative_addr / granularity;
    size_t num_targets = interleave_config_.target_ids.size();
    size_t target_index = block_index % num_targets;
    uint32_t target_id = interleave_config_.target_ids[target_index];

    // Calculate local offset: which block within this target's portion
    uint64_t target_block = block_index / num_targets;
    uint64_t intra_block_offset = relative_addr % granularity;
    uint64_t local_offset = target_block * granularity + intra_block_offset;

    // Determine if remote by checking ranges (if configured)
    bool is_remote = false;
    for (const auto& range : ranges_) {
        if (range.target_id == target_id) {
            is_remote = range.is_remote;
            break;
        }
    }

    uint32_t hops = is_remote ? 1 : 0;
    return {target_id, local_offset, is_remote, hops};
}

uint32_t HDMDecoder::get_home_node(uint64_t addr) const {
    auto result = decode(addr);
    return result.target_id;
}

bool HDMDecoder::is_local(uint64_t addr, uint32_t local_node_id) const {
    auto result = decode(addr);
    return result.target_id == local_node_id && !result.is_remote;
}
