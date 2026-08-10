/*
 * Damer Hardware-JIT policy descriptors embedded in the CXLMemSim switch path.
 *
 * The SystemVerilog source under fpga/ is the hardware policy artifact. This
 * header is the C++ behavioral twin used by the switch runtime model.
 */

#ifndef DAMER_HWJIT_POLICY_H
#define DAMER_HWJIT_POLICY_H

#include <array>
#include <cstdint>

enum DamerHwjitTransform : uint32_t {
    DAMER_HWJIT_MOVE = 0,
    DAMER_HWJIT_QUANTIZE = 1u << 0,
    DAMER_HWJIT_COMPRESS = 1u << 1,
    DAMER_HWJIT_CHECKSUM = 1u << 2,
    DAMER_HWJIT_FILTER = 1u << 3,
    DAMER_HWJIT_REDUCE = 1u << 4,
    DAMER_HWJIT_SCATTER_GATHER = 1u << 5,
    DAMER_HWJIT_REPLICATE = 1u << 6,
    DAMER_HWJIT_PERSIST = 1u << 7,
};

enum DamerQwen27BHwjitSwitchlet : uint32_t {
    DAMER_QWEN27B_LOGITS_MOVE = 0,
    DAMER_QWEN27B_KV_PACK = 1,
    DAMER_QWEN27B_PREFILL_ACTIVATION_SPILL = 2,
    DAMER_QWEN27B_DECODE_KV_FETCH = 3,
    DAMER_QWEN27B_ATTENTION_MASK_FILTER = 4,
    DAMER_QWEN27B_TP_LOGITS_REDUCE = 5,
    DAMER_QWEN27B_TENSOR_SHARD_EXCHANGE = 6,
    DAMER_QWEN27B_KV_REPLICA_REFRESH = 7,
    DAMER_QWEN27B_CHECKPOINT_PERSIST = 8,
};

struct DamerHwjitSwitchletSpec {
    uint32_t id;
    uint32_t transform_mask;
    uint32_t default_ttl;
    uint32_t default_max_ops;
    const char *name;
    const char *rtl_module;
};

constexpr std::array<DamerHwjitSwitchletSpec, 9> kDamerQwen27BHwjitSwitchlets = {{
    {DAMER_QWEN27B_LOGITS_MOVE,
     DAMER_HWJIT_MOVE,
     4,
     1,
     "qwen27b_logits_move",
     "damer_cxl_switch_hwjit_qwen27b_logits_move"},
    {DAMER_QWEN27B_KV_PACK,
     DAMER_HWJIT_QUANTIZE,
     4,
     1,
     "qwen27b_kv_pack",
     "damer_cxl_switch_hwjit_qwen27b_kv_pack"},
    {DAMER_QWEN27B_PREFILL_ACTIVATION_SPILL,
     DAMER_HWJIT_COMPRESS,
     4,
     1,
     "qwen27b_prefill_activation_spill",
     "damer_cxl_switch_hwjit_qwen27b_prefill_activation_spill"},
    {DAMER_QWEN27B_DECODE_KV_FETCH,
     DAMER_HWJIT_CHECKSUM,
     4,
     1,
     "qwen27b_decode_kv_fetch",
     "damer_cxl_switch_hwjit_qwen27b_decode_kv_fetch"},
    {DAMER_QWEN27B_ATTENTION_MASK_FILTER,
     DAMER_HWJIT_FILTER,
     4,
     1,
     "qwen27b_attention_mask_filter",
     "damer_cxl_switch_hwjit_qwen27b_attention_mask_filter"},
    {DAMER_QWEN27B_TP_LOGITS_REDUCE,
     DAMER_HWJIT_REDUCE,
     4,
     1,
     "qwen27b_tp_logits_reduce",
     "damer_cxl_switch_hwjit_qwen27b_tp_logits_reduce"},
    {DAMER_QWEN27B_TENSOR_SHARD_EXCHANGE,
     DAMER_HWJIT_SCATTER_GATHER,
     4,
     1,
     "qwen27b_tensor_shard_exchange",
     "damer_cxl_switch_hwjit_qwen27b_tensor_shard_exchange"},
    {DAMER_QWEN27B_KV_REPLICA_REFRESH,
     DAMER_HWJIT_REPLICATE,
     4,
     1,
     "qwen27b_kv_replica_refresh",
     "damer_cxl_switch_hwjit_qwen27b_kv_replica_refresh"},
    {DAMER_QWEN27B_CHECKPOINT_PERSIST,
     DAMER_HWJIT_PERSIST,
     4,
     1,
     "qwen27b_checkpoint_persist",
     "damer_cxl_switch_hwjit_qwen27b_checkpoint_persist"},
}};

constexpr const DamerHwjitSwitchletSpec *damer_hwjit_lookup(uint32_t switchlet_id) {
    for (const auto &spec : kDamerQwen27BHwjitSwitchlets) {
        if (spec.id == switchlet_id) {
            return &spec;
        }
    }
    return nullptr;
}

#endif // DAMER_HWJIT_POLICY_H
