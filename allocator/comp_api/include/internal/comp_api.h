#ifndef COMP_API_H_
#define COMP_API_H_

#include "opt_api/include/internal/opt_api.h"
#include "opt_api/include/internal/config.h"

int get_current_prio(void);
mem_zone_t get_cur_prioritized_memtype(void);
int update_arena_pool(int prio, size_t allocated);

#define MAP_JEMALLOC_INTERNAL_MMAP 0x400000
#define RET_USE_EXMEM_FALSE (1)
typedef void *(*mmap_ptr_t)(void *, size_t, int, int, int, off_t);
typedef struct {
    mmap_ptr_t orig_mmap;
    bool is_initialized;
    unsigned char calloc_buf[4096];
} tr_syscall_config;
extern tr_syscall_config opt_syscall;
bool is_tr_syscall_initialized(void);
int init_mmap_ptr(void);
int get_prio_by_type(mem_zone_t type);
bool is_cur_prio_exmem(int *prio);
bool is_cxlmalloc_initialized(void);
int init_cxlmalloc(void);
size_t malloc_usable_size_internal (mem_zone_t type, void *ptr);

#define CXLMALLOC_CONSTRUCTOR_PRIORITY (SMALLOC_CONSTRUCTOR_PRIORITY-1)

#define CXLMALLOC_PRECONDITION(n) do {\
		if (unlikely(is_cxlmalloc_initialized() == false)) {\
			if (init_cxlmalloc() == SMDK_RET_USE_EXMEM_FALSE) {\
				return je_##n;\
			}\
		}\
	}while (0)

#define MALLOC_JEMALLOC(s) je_malloc(s)
#define CALLOC_JEMALLOC(n, s) je_calloc(n, s)
#define REALLOC_JEMALLOC(p, s) je_realloc(p, s)
#define POSIX_MEMALIGN_JEMALLOC(m, a, s) je_posix_memalign(m, a, s)
#define FREE_JEMALLOC(p) je_free(p)

#endif /* COMP_API_H_ */
