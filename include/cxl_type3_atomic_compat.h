/*
 * Source-compatible redirection of C11 64-bit atomic_fetch_add_explicit().
 *
 * Include this after <stdatomic.h>, or compile with:
 *
 *   -include cxl_type3_atomic_compat.h
 *
 * The expression remains atomic_fetch_add_explicit() in application source.
 * DAX pointers are sent through the CXLMemSim Type-3 atomic BAR; ordinary
 * pointers retain native compiler-atomic behavior.
 */
#ifndef CXLMEMSIM_TYPE3_ATOMIC_COMPAT_H
#define CXLMEMSIM_TYPE3_ATOMIC_COMPAT_H

#ifdef __cplusplus
#error "cxl_type3_atomic_compat.h currently supports C11 atomics only"
#endif

#include <stdatomic.h>
#include <stdint.h>

#include "cxl_type3_atomic.h"

#ifdef atomic_fetch_add_explicit
#undef atomic_fetch_add_explicit
#endif

/* GNU statement expressions make every argument evaluate exactly once. */
#define atomic_fetch_add_explicit(object, operand, order)                                                              \
    __extension__({                                                                                                    \
        __auto_type cxl_atomic_object_local = (object);                                                                \
        _Static_assert(sizeof(*cxl_atomic_object_local) == sizeof(uint64_t),                                           \
                       "CXLMemSim automatic fetch_add currently supports only 64-bit atomics");                        \
        uint64_t cxl_atomic_operand_local = (uint64_t)(operand);                                                       \
        int cxl_atomic_order_local = (int)(order);                                                                     \
        cxl_type3_atomic_fetch_add_auto_u64((volatile void *)cxl_atomic_object_local, cxl_atomic_operand_local,        \
                                            cxl_atomic_order_local);                                                   \
    })

#endif /* CXLMEMSIM_TYPE3_ATOMIC_COMPAT_H */
