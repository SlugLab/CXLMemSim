#ifndef CXLMALLOC_H_
#define CXLMALLOC_H_

#include "core/include/internal/init.h"
#include "core/include/internal/config.h"
#include "core/include/internal/alloc.h"

extern mem_zone_t get_cur_prioritized_memtype(void);
extern bool is_tr_syscall_initialized(void);
extern int update_arena_pool(int prio, size_t allocated);
extern int init_mmap_ptr(void);
extern int get_prio_by_type(mem_zone_t type);
extern bool is_cur_prio_exmem(int *prio);
extern int init_cxlmalloc(void);
extern size_t malloc_usable_size_internal (mem_zone_t type, void *ptr);
extern bool is_use_exmem(void);
extern bool is_cxlmalloc_initialized(void);
extern int get_current_prio(void);

#define CXLMALLOC_PRECONDITION(n) do {				    \
        if (unlikely(!is_use_exmem())) {			    \
            return je_##n;					    \
        } else if (unlikely(is_cxlmalloc_initialized() == false)) { \
            return je_##n;					    \
        }							    \
    }while (0)
#define MAP_JEMALLOC_INTERNAL_MMAP 0x800000
#define RET_USE_EXMEM_FALSE (1)
#define CXLMALLOC_CONSTRUCTOR_PRIORITY (SMALLOC_CONSTRUCTOR_PRIORITY-1)
typedef void *(*mmap_ptr_t)(void *, size_t, int, int, int, off_t);

typedef struct {
    mmap_ptr_t orig_mmap;
    bool is_initialized;
    unsigned char calloc_buf[4096];
} tr_syscall_config;
extern tr_syscall_config opt_syscall;

#endif /* CXLMALLOC_H_ */
