/*
 * CXLMemSim WASM bridge — C ABI between the SharedWorker (JS) and
 * the compiled cxlmemsim core. The JS side calls these symbols via
 * the emscripten `Module._cxlmemsim_*` entry points.
 *
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 */
#ifndef CXLMEMSIM_WASM_BRIDGE_H
#define CXLMEMSIM_WASM_BRIDGE_H

#include <stdint.h>

/* 256-byte fixed-layout stats block returned by cxlmemsim_get_stats. */
typedef struct {
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t total_atomics;
    uint64_t total_invalidations;
    uint64_t total_errors;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t total_latency_ns;
    uint64_t pool_capacity_bytes;
    uint64_t mesi_invalid;
    uint64_t mesi_shared;
    uint64_t mesi_exclusive;
    uint64_t mesi_modified;
    uint64_t reserved[19];
} cxlmemsim_stats_t;

#ifdef __cplusplus
static_assert(sizeof(cxlmemsim_stats_t) == 256,
              "cxlmemsim_stats_t layout changed — update JS offsets in cxlmemsim-pool-worker.js");
#endif

#ifdef __cplusplus
extern "C" {
#endif

int cxlmemsim_init(uint32_t pool_capacity_bytes,
                   const char *topology_json,
                   uint32_t *out_pool_base);

int32_t cxlmemsim_handle_request(uint32_t req_ptr,
                                 uint32_t resp_ptr,
                                 uint32_t inv_out_ptr,
                                 uint32_t inv_cap);

void cxlmemsim_handle_type2(uint32_t msg_ptr);

void cxlmemsim_get_stats(uint32_t out_ptr);

void cxlmemsim_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* CXLMEMSIM_WASM_BRIDGE_H */
