#include "core/include/internal/alloc.h"
#include "internal/cxlmalloc.h"
#include "jemalloc/jemalloc.h"

SMDK_EXPORT
void *malloc(size_t size) {
    CXLMALLOC_PRECONDITION(malloc(size));
    mem_zone_t type = get_cur_prioritized_memtype();
    return s_malloc_internal(type, size, false);
}

SMDK_EXPORT
void *calloc(size_t num, size_t size) {
    if (unlikely(!is_tr_syscall_initialized())) {
        return (void *)opt_syscall.calloc_buf;
    }

    CXLMALLOC_PRECONDITION(calloc(num, size));
    mem_zone_t type = get_cur_prioritized_memtype();
    return s_malloc_internal(type, num * size, true);
}

SMDK_EXPORT
void *realloc(void *ptr, size_t size) {
    CXLMALLOC_PRECONDITION(realloc(ptr, size));
    mem_zone_t type = get_cur_prioritized_memtype();
    return s_realloc_internal(type, ptr, size);
}

SMDK_EXPORT
int posix_memalign(void **memptr, size_t alignment, size_t size) {
    CXLMALLOC_PRECONDITION(posix_memalign(memptr, alignment, size));
    mem_zone_t type = get_cur_prioritized_memtype();
    return s_posix_memalign_internal(type, memptr, alignment, size);
}

SMDK_EXPORT
void *aligned_alloc(size_t alignment, size_t size) {
    CXLMALLOC_PRECONDITION(aligned_alloc(alignment, size));
    mem_zone_t type = get_cur_prioritized_memtype();
    return s_aligned_alloc_internal(type, alignment, size);
}

SMDK_EXPORT
void free(void *ptr) {
    if (unlikely(ptr == (void *)opt_syscall.calloc_buf)) {
        return;
    }

    CXLMALLOC_PRECONDITION(free(ptr));
    return s_free_internal(ptr);
}

SMDK_EXPORT
void *mmap(void *start, size_t len, int prot, int flags, int fd, off_t off) {
    void *ret = NULL;
    int mmap_initialized = init_mmap_ptr();

    if (unlikely(mmap_initialized != SMDK_RET_SUCCESS)) {
        fprintf(stderr, "init_mmap_ptr() failed\n");
        return ret;
    }
    if (likely(smdk_info.smdk_initialized)) {
        int prio;
        if (likely(flags & MAP_JEMALLOC_INTERNAL_MMAP)) {
            /* mmap called by internal logic */
            if (flags & MAP_EXMEM) {
                prio = get_prio_by_type(mem_zone_exmem);
            } else {
                prio = get_prio_by_type(mem_zone_normal);
            }
        } else {
            /* mmap called by user */
            if (is_cur_prio_exmem(&prio)) {
                flags |= MAP_EXMEM;
            } else {
                flags |= MAP_NORMAL;
            }
        }
        set_interleave_policy(flags);
        ret = opt_syscall.orig_mmap(start, len, prot, flags, fd, off);
        if (likely(ret)) {
            /* update_arena_pool only after smdk has been initialized */
            update_arena_pool(prio, len);
        }
    } else {
        ret = opt_syscall.orig_mmap(start, len, prot, flags, fd, off);
    }

    return ret;
}

SMDK_EXPORT
size_t malloc_usable_size (void *ptr) { /* added for redis */
    mem_zone_t type = get_memtype_from_arenaidx(je_arenaidx_find(ptr));
    return malloc_usable_size_internal(type, ptr);
}

SMDK_CONSTRUCTOR(CXLMALLOC_CONSTRUCTOR_PRIORITY)
static void
cxlmalloc_constructor(void) {
    init_cxlmalloc();
    show_smdk_info(false);
}
