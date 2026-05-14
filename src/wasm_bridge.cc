#include "wasm_bridge.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#define KEEPALIVE
#endif

extern "C" {

KEEPALIVE int cxlmemsim_init(uint32_t /*pool_capacity_bytes*/,
                             const char * /*topology_json*/,
                             uint32_t *out_pool_base) {
    if (out_pool_base) {
        *out_pool_base = 0;
    }
    return -1; /* unimplemented in this task */
}

KEEPALIVE int32_t cxlmemsim_handle_request(uint32_t /*req_ptr*/,
                                           uint32_t /*resp_ptr*/,
                                           uint32_t /*inv_out_ptr*/,
                                           uint32_t /*inv_cap*/) {
    return -1;
}

KEEPALIVE void cxlmemsim_handle_type2(uint32_t /*msg_ptr*/) {}

KEEPALIVE void cxlmemsim_get_stats(uint32_t /*out_ptr*/) {}

KEEPALIVE void cxlmemsim_reset(void) {}

} /* extern "C" */
