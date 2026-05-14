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
