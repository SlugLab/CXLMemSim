#ifndef ALLOC_H_
#define ALLOC_H_

#include "internal/init.h"

extern void *s_malloc_internal(mem_zone_t type, size_t size, bool zeroed);
extern void *s_realloc_internal(mem_zone_t type, void *ptr, size_t size);
extern int s_posix_memalign_internal(mem_zone_t type, void **memptr, size_t alignment, size_t size);
extern void *s_aligned_alloc_internal(mem_zone_t type, size_t alignment, size_t size);
extern void s_free_internal_type(mem_zone_t type, void *ptr);
extern void s_free_internal(void *ptr);
extern bool is_memtype_valid(mem_zone_t memtype);
extern mem_zone_t get_memtype_from_arenaidx(unsigned idx);

#endif /* ALLOC_H_ */
