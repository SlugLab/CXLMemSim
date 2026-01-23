/*
 * CXLMemSim HDM (Host-managed Device Memory) Decoder
 *
 * Implements CXL-spec address decoding for multi-device topologies.
 * Supports range-based, interleaved, and hybrid address mapping modes.
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 * Copyright 2025 Regents of the University of California
 * UC Santa Cruz Sluglab.
 */

#ifndef CXLMEMSIM_HDM_DECODER_H
#define CXLMEMSIM_HDM_DECODER_H

#include <cstdint>
#include <vector>
#include <algorithm>

enum class HDMDecoderMode { INTERLEAVED, RANGE_BASED, HYBRID };

enum class InterleaveGranularity : uint64_t {
    CACHELINE_64B  = 64,
    CACHELINE_256B = 256,
    PAGE_4K        = 4096,
    PAGE_2M        = 2097152,
    PAGE_1G        = 1073741824
};

struct HDMRange {
    uint64_t base_addr;
    uint64_t size;
    uint32_t target_id;      // Device ID or node ID
    bool is_remote;
};

struct HDMInterleaveConfig {
    InterleaveGranularity granularity = InterleaveGranularity::CACHELINE_256B;
    std::vector<uint32_t> target_ids;    // Ordered targets in interleave set
    uint64_t base_addr = 0;
    uint64_t total_size = 0;
};

class HDMDecoder {
public:
    struct DecodeResult {
        uint32_t target_id;
        uint64_t local_offset;   // Offset within target's memory
        bool is_remote;
        uint32_t hop_count;
    };

    explicit HDMDecoder(HDMDecoderMode mode);

    void add_range(uint64_t base, uint64_t size, uint32_t target_id, bool is_remote = false);
    void configure_interleave(InterleaveGranularity gran,
                              const std::vector<uint32_t>& targets,
                              uint64_t base, uint64_t total_size);

    DecodeResult decode(uint64_t addr) const;
    uint32_t get_home_node(uint64_t addr) const;
    bool is_local(uint64_t addr, uint32_t local_node_id) const;

    HDMDecoderMode get_mode() const { return mode_; }
    size_t num_ranges() const { return ranges_.size(); }

private:
    HDMDecoderMode mode_;
    std::vector<HDMRange> ranges_;
    HDMInterleaveConfig interleave_config_;
    bool ranges_sorted_ = false;

    DecodeResult decode_range(uint64_t addr) const;
    DecodeResult decode_interleaved(uint64_t addr) const;
    void sort_ranges();
};

#endif // CXLMEMSIM_HDM_DECODER_H
