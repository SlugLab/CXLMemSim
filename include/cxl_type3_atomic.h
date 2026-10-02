/*
 * Guest-side API for the experimental CXLMemSim Type-3 atomic command BAR.
 *
 * These calls operate on a device-relative DPA byte offset.  They are the
 * supported way to request a server-serialized RMW; C11 atomics applied
 * directly to a callback-backed /dev/dax mapping cannot be reconstructed from
 * KVM's separate MMIO read and write exits.
 */
#ifndef CXLMEMSIM_TYPE3_ATOMIC_H
#define CXLMEMSIM_TYPE3_ATOMIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cxl_type3_atomic cxl_type3_atomic_t;

/* Open a specific PCI resource file, normally .../<BDF>/resource5. */
int cxl_type3_atomic_open_resource(const char *resource_path, cxl_type3_atomic_t **out);

/* Open resource5 for a CXL Type-3 device identified by PCI BDF. */
int cxl_type3_atomic_open_bdf(const char *bdf, cxl_type3_atomic_t **out);

/*
 * Find the first Intel 8086:0d93 Type-3 device exposing the atomic ABI.
 * CXL_TYPE3_ATOMIC_RESOURCE or CXL_TYPE3_ATOMIC_BDF overrides discovery.
 */
int cxl_type3_atomic_open_auto(cxl_type3_atomic_t **out);

void cxl_type3_atomic_close(cxl_type3_atomic_t *ctx);

/* Atomic DPAs must be naturally aligned to 8 bytes. */
int cxl_type3_atomic_fetch_add_u64(cxl_type3_atomic_t *ctx, uint64_t dpa, uint64_t value, uint64_t *old_value);
/* Returns command status; CAS succeeded when *old_value == expected. */
int cxl_type3_atomic_compare_exchange_u64(cxl_type3_atomic_t *ctx, uint64_t dpa, uint64_t expected, uint64_t desired,
                                          uint64_t *old_value);
int cxl_type3_atomic_fence(cxl_type3_atomic_t *ctx);

/* Raw status returned by CXLMemSim for the most recent command. */
uint32_t cxl_type3_atomic_last_server_status(const cxl_type3_atomic_t *ctx);

/*
 * Backend for cxl_type3_atomic_compat.h.  If object is in a /dev/dax mapping,
 * the mapping's file offset is translated to a DPA and BAR5 is used.  Other
 * pointers use a native compiler atomic.  Failures on a DAX pointer are fatal:
 * silently falling back would lose cross-VM atomicity.
 */
uint64_t cxl_type3_atomic_fetch_add_auto_u64(volatile void *object, uint64_t value, int memory_order);

/*
 * Override automatic /proc/self/maps discovery, primarily for custom mappings
 * and tests.  The context remains owned by the caller.  Pass NULL to clear.
 */
int cxl_type3_atomic_auto_bind(cxl_type3_atomic_t *ctx, const void *mapping_base, size_t mapping_length,
                               uint64_t dpa_base);

#ifdef __cplusplus
}
#endif

#endif /* CXLMEMSIM_TYPE3_ATOMIC_H */
