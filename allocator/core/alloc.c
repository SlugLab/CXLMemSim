/*
   Copyright, Samsung Corporation

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

 * Redistributions of source code must retain the above copyright
 notice, this list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright
 notice, this list of conditions and the following disclaimer in
 the documentation and/or other materials provided with the
 distribution.

 * Neither the name of the copyright holder nor the names of its
 contributors may be used to endorse or promote products derived
 from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  */

#include "internal/init.h"
#include "internal/alloc.h"
#include "internal/config.h"
#include "jemalloc/jemalloc.h"
#include <sys/sysinfo.h>

SMDK_INLINE
unsigned get_arena_idx(mem_zone_t memtype) {
    return smdk_info.get_target_arena(memtype);
}

inline bool is_memtype_valid(mem_zone_t memtype) {
    return ((memtype == mem_zone_normal) || (memtype == mem_zone_exmem));
}

inline mem_zone_t get_memtype_from_arenaidx(unsigned idx) {
    if (unlikely(idx == -1)) {
        return mem_zone_invalid;
    }
    if (idx < g_arena_pool[1].arena_id[0]) {
        return opt_smdk.prio[0];
    } else {
        return opt_smdk.prio[1];
    }
}

inline void *s_malloc_internal(mem_zone_t type, size_t size, bool zeroed) {
    if (unlikely(!is_memtype_valid(type))) return NULL;

    set_tsd_set_cur_mem_type(type);
    int flags = MALLOCX_ARENA(get_arena_idx(type));
    if (zeroed) {
        flags |= MALLOCX_ZERO;
    }
    return je_mallocx(size, flags);
}

inline void *s_realloc_internal(mem_zone_t type, void *ptr, size_t size) {
    if (unlikely(!is_memtype_valid(type))) return NULL;

    if (unlikely(size == 0 && ptr != NULL)) { //call free
        mem_zone_t old_type = get_memtype_from_arenaidx(je_arenaidx_find(ptr));
        if (likely(old_type != mem_zone_invalid)) {
            set_tsd_set_cur_mem_type(old_type);
            je_free(ptr);
        }
        return NULL;
    } else {
        if (unlikely(ptr == NULL)) {
            int flags = MALLOCX_ARENA(get_arena_idx(type));
            set_tsd_set_cur_mem_type(type);
            return je_mallocx(size, flags);
        } else {
            mem_zone_t old_type = get_memtype_from_arenaidx(je_arenaidx_find(ptr));
            if (unlikely(old_type == mem_zone_invalid)) { //invalid ptr
                return NULL;
            } else if (likely(type == old_type)) {
                int flags = MALLOCX_ARENA(get_arena_idx(type));
                set_tsd_set_cur_mem_type(type);
                return je_rallocx(ptr, size, flags);
            } else { // type != old_type
                int flags = MALLOCX_ARENA(get_arena_idx(type));
                set_tsd_set_cur_mem_type(type);
                void* ret = je_mallocx(size, flags);
                set_tsd_set_cur_mem_type(old_type);
                if (likely(ret)) {
                    size_t old_size = je_usize_find(ptr);
                    size_t copy_size = (size < old_size) ? size: old_size;
                    memcpy(ret, ptr, copy_size);
                }
                je_free(ptr);
                return ret;
            }
        }
    }
}

inline int s_posix_memalign_internal(mem_zone_t type, void **memptr, size_t alignment, size_t size) {
    if (unlikely(!is_memtype_valid(type))) return EINVAL;

    set_tsd_set_cur_mem_type(type);
    *memptr = NULL;
    int flags = MALLOCX_ARENA(get_arena_idx(type)) | MALLOCX_ALIGN(alignment);

    *memptr = je_mallocx(size, flags);
    if (*memptr == NULL) {
        return ENOMEM;
    }
    return 0;
}

inline void *s_aligned_alloc_internal(mem_zone_t type, size_t alignment, size_t size) {
    if (unlikely(!is_memtype_valid(type))) {
        set_errno(EINVAL);
        return NULL;
    }
    set_tsd_set_cur_mem_type(type);
    int flags = MALLOCX_ARENA(get_arena_idx(type)) | MALLOCX_ALIGN(alignment);
    return je_mallocx(size, flags);
}

inline void s_free_internal(void *ptr) {
    if (unlikely(ptr == NULL)) return;

    mem_zone_t type = get_memtype_from_arenaidx(je_arenaidx_find(ptr));
    if (unlikely(!is_memtype_valid(type))) return;

    set_tsd_set_cur_mem_type(type);
    je_free(ptr);
}

inline void s_free_internal_type(mem_zone_t type, void *ptr) {
    if (unlikely(ptr == NULL)) return;
    if (unlikely(!is_memtype_valid(type))) return;

    set_tsd_set_cur_mem_type(type);
    mem_zone_t ptr_type = get_memtype_from_arenaidx(je_arenaidx_find(ptr));
    if (unlikely(!is_memtype_valid(ptr_type))) return;

    set_tsd_set_cur_mem_type(ptr_type);
    je_free(ptr);
}
