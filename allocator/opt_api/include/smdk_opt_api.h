/*
 *  SMDK optimization APIs for heap memory management.
 */
#ifndef SMDK_OPT_API_H_
#define SMDK_OPT_API_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

typedef enum {
    SMDK_MEM_NORMAL,
    SMDK_MEM_EXMEM
} smdk_memtype_t;

/* HEAP MANAGEMENT INTERFACE */

void *s_malloc(smdk_memtype_t type, size_t size);

void *s_calloc(smdk_memtype_t type, size_t num, size_t size);

void *s_realloc(smdk_memtype_t type, void *ptr, size_t size);

int s_posix_memalign(smdk_memtype_t type, void **memptr, size_t alignment, size_t size);

void s_free(void *ptr);

void s_free_type(smdk_memtype_t type, void *ptr);

size_t s_get_memsize_total(smdk_memtype_t type);

size_t s_get_memsize_used(smdk_memtype_t type);

size_t s_get_memsize_available(smdk_memtype_t type);

size_t s_get_memsize_node_total(smdk_memtype_t type, int node);

size_t s_get_memsize_node_available(smdk_memtype_t type, int node);

void s_stats_print(char unit);

void s_stats_node_print(char unit);

void s_enable_node_interleave(char* nodes);

void s_disable_node_interleave(void);

void *s_malloc_node(smdk_memtype_t type, size_t size, char* nodes);

void s_free_node(smdk_memtype_t type, void* mem, size_t size);


#ifdef __cplusplus
}
#endif
#endif /* SMDK_OPT_API_H_ */

