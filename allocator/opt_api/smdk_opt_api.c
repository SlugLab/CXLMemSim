#include "smdk_opt_api.h"
#include "core/include/internal/alloc.h"
#include "core/include/internal/config.h"
#include "internal/smalloc.h"
#include "internal/meta_api.h"
#include <numa.h>
#include <sched.h>

SMDK_EXPORT
void * s_malloc(smdk_memtype_t type, size_t size) {
    return s_malloc_internal(type, size, false);
}

SMDK_EXPORT
void* s_calloc(smdk_memtype_t type, size_t num, size_t size) {
    return s_malloc_internal(type, num * size, true);
}

SMDK_EXPORT
void* s_realloc(smdk_memtype_t type, void *ptr, size_t size) {
    return s_realloc_internal(type, ptr, size);
}

SMDK_EXPORT
int s_posix_memalign(smdk_memtype_t type, void **memptr, size_t alignment, size_t size) {
    return s_posix_memalign_internal(type, memptr, alignment, size);
}

SMDK_EXPORT
void s_free(void *ptr) {
    return s_free_internal(ptr);
}

SMDK_EXPORT
void s_free_type(smdk_memtype_t type, void *ptr) {
    return s_free_internal_type(type, ptr);
}

SMDK_EXPORT
size_t s_get_memsize_total(smdk_memtype_t type) { //metaAPI
    if (unlikely(!is_memtype_valid(type))) return INVALID_MEM_SIZE;
    return get_mem_stats_total(type);
}

SMDK_EXPORT
size_t s_get_memsize_used(smdk_memtype_t type) { //metaAPI
    if (unlikely(!is_memtype_valid(type))) return INVALID_MEM_SIZE;
    return get_mem_stats_used(type);
}

SMDK_EXPORT
size_t s_get_memsize_available(smdk_memtype_t type) { //metaAPI
    if (unlikely(!is_memtype_valid(type))) return INVALID_MEM_SIZE;
    return get_mem_stats_available(type);
}

SMDK_EXPORT
size_t s_get_memsize_node_total(smdk_memtype_t type, int node) { //metaAPI
    if (unlikely(!is_memtype_valid(type))) return INVALID_MEM_SIZE;
    return get_mem_stats_node_total(type, node);
}

SMDK_EXPORT
size_t s_get_memsize_node_available(smdk_memtype_t type, int node) { //metaAPI
    if (unlikely(!is_memtype_valid(type))) return INVALID_MEM_SIZE;
    return get_mem_stats_node_available(type, node);
}

SMDK_EXPORT
void s_stats_print(char unit) {
    mem_stats_print(unit);
}

SMDK_EXPORT
void s_stats_node_print(char unit) {
    stats_per_node_print(unit);
}

SMDK_EXPORT
void s_enable_node_interleave(char *nodes){
    struct bitmask *nodemask_parsed = numa_parse_nodestring(nodes);
    if (likely(nodemask_parsed != 0)) {
        numa_set_interleave_mask(nodemask_parsed);
    } else {
        fprintf(stderr, "[Warning] %s:invalid node(s).(%s)\n", __FUNCTION__, nodes);
    }
}

SMDK_EXPORT
void s_disable_node_interleave(void){
    numa_set_interleave_mask(numa_no_nodes_ptr);
}

SMDK_EXPORT
void* s_malloc_node(smdk_memtype_t type, size_t size, char *nodes) {
    void *mem;
    struct bitmask *original_bitmask = numa_get_interleave_mask();
    struct bitmask *nodemask_parsed = numa_parse_nodestring(nodes);
    if (unlikely(nodemask_parsed == 0)) {
        fprintf(stderr, "[Warning] %s:invalid node(s).(%s)\n", __FUNCTION__, nodes);
        return NULL;
    }
    int flags = MAP_PRIVATE|MAP_ANON|MAP_POPULATE;
    int core = sched_getcpu();
    if (type == SMDK_MEM_EXMEM) {
        flags |= MAP_EXMEM;
        smdk_info.node_alloc_stat_exmem[core]+=(long long)size;
    } else {
        flags |= MAP_NORMAL;
        smdk_info.node_alloc_stat_normal[core]+=(long long)size;
    }
    numa_set_interleave_mask(nodemask_parsed);
    mem = mmap(0, size, PROT_READ|PROT_WRITE, flags, 0, 0);
    assert(mem != MAP_FAILED);

    numa_set_interleave_mask(original_bitmask);


    return mem;
}

SMDK_EXPORT
void s_free_node(smdk_memtype_t type, void* mem, size_t size){
    munmap(mem, size);

    int core = sched_getcpu();
    if (type == SMDK_MEM_EXMEM) {
        smdk_info.node_alloc_stat_exmem[core]-=(long long)size;
    } else {
        smdk_info.node_alloc_stat_normal[core]-=(long long)size;
    }
}


SMDK_CONSTRUCTOR(SMALLOC_CONSTRUCTOR_PRIORITY)
static void smalloc_constructor(void) {
    init_smalloc();
    show_smdk_info(true);
}
