#include "internal/comp_api.h"
#include "jemalloc/jemalloc.h"
#include <dlfcn.h>

inline bool is_cxlmalloc_initialized(void) {
	return smdk_info.smdk_initialized;
}

int init_cxlmalloc(void) {
	if (unlikely(opt_smdk.use_exmem == false)) {
		return SMDK_RET_USE_EXMEM_FALSE;
	}

	smdk_init_helper("CXLMALLOC_CONF", false);
	int ret = init_smdk();

	if (init_mmap_ptr() == SMDK_RET_INIT_MMAP_PTR_FAIL) {
		abort();
	}

	if (ret == SMDK_RET_USE_EXMEM_FALSE) {
        fprintf(stderr, "[Warning] \"use_exmem:false\". Jemalloc would be used instead.\n");
	}

	return ret;
}

inline int get_prio_by_type(mem_zone_t type) {
	return (opt_smdk.prio[0] == type)?0:1;
}

inline int get_current_prio(void){
    int prio;
    pthread_rwlock_rdlock(&smdk_info.rwlock_current_prio);
    prio = smdk_info.current_prio;
    pthread_rwlock_unlock(&smdk_info.rwlock_current_prio);

	return prio;
}

inline mem_zone_t get_cur_prioritized_memtype(void){
	return opt_smdk.prio[get_current_prio()];
}

static int do_maxmemory_policy(arena_pool* pool){
    switch(smdk_info.maxmemory_policy){
        case policy_oom:
            fprintf(stderr,"%s: terminated - maxmemory_policy=oom, current_prio=%d zone_allocated=%ld zone_limit=%ld\n",
                    __FUNCTION__,smdk_info.current_prio, pool->zone_allocated, pool->zone_limit);
            assert(false);
            break;

        case policy_interleave:
            /* reset allocated size (pool 0/1) */
            pthread_rwlock_wrlock(&g_arena_pool[0].rwlock_zone_allocated);
            g_arena_pool[0].zone_allocated = 0;
            pthread_rwlock_unlock(&g_arena_pool[0].rwlock_zone_allocated);

            pthread_rwlock_wrlock(&g_arena_pool[1].rwlock_zone_allocated);
            g_arena_pool[1].zone_allocated = 0;
            pthread_rwlock_unlock(&g_arena_pool[1].rwlock_zone_allocated);

            /* change priority 1-> 0 */
            pthread_rwlock_wrlock(&smdk_info.rwlock_current_prio);
            smdk_info.current_prio = 0;
            pthread_rwlock_unlock(&smdk_info.rwlock_current_prio);

            fprintf(stderr,"%s: CHANGE Priority 1->0\n",__FUNCTION__);
            break;

        case policy_remain:
            /* reset allocated size (current priority, 1) */
            pthread_rwlock_wrlock(&pool->rwlock_zone_allocated);
            pool->zone_allocated = 0;
            pthread_rwlock_unlock(&pool->rwlock_zone_allocated);

            fprintf(stderr,"%s:  REMAIN 1->1\n",__FUNCTION__);
            break;

        default:
            fprintf(stderr,"%s:  invalid policy %d\n",__FUNCTION__,smdk_info.maxmemory_policy);
            assert(false);
    }
    return 0;
}

inline int update_arena_pool(int prio, size_t allocated){

    arena_pool* pool = &g_arena_pool[prio];

    pthread_rwlock_wrlock(&pool->rwlock_zone_allocated);
    pool->zone_allocated += allocated; /* bytes */
    pthread_rwlock_unlock(&pool->rwlock_zone_allocated);

    /*
    fprintf(stderr,"%s: arena_pool[%d].zone_allocated=%ld zone_limit=%ld\n",__FUNCTION__,
            smdk_info.current_prio, pool->zone_allocated, pool->zone_limit);
            */

    pthread_rwlock_rdlock(&pool->rwlock_zone_allocated);
    if(pool->zone_allocated > (pool->zone_limit)*MB){ /* zone_limit: MB */
        pthread_rwlock_unlock(&pool->rwlock_zone_allocated);
        if(smdk_info.current_prio == 0){
            pthread_rwlock_wrlock(&smdk_info.rwlock_current_prio);
            smdk_info.current_prio = 1;
            pthread_rwlock_unlock(&smdk_info.rwlock_current_prio);
            fprintf(stderr,"%s: CHANGE Priority 0->1\n", "update_arena_pool");
        }
        else{
            do_maxmemory_policy(pool);
        }
        if (opt_smdk.use_auto_arena_scaling == false) {
            pthread_rwlock_wrlock(&pool->rwlock_arena_index);
            pool->arena_index++;
            pthread_rwlock_unlock(&pool->rwlock_arena_index);
        }
    }
    else{
        pthread_rwlock_unlock(&pool->rwlock_zone_allocated);
    }
    return 0;
}

tr_syscall_config opt_syscall = {
    .orig_mmap = NULL,
    .is_initialized = false,
};

inline bool is_tr_syscall_initialized(void){
    return opt_syscall.is_initialized;
}

inline int init_mmap_ptr(void){
    if (is_tr_syscall_initialized()) {
        return SMDK_RET_SUCCESS;
    }

    opt_syscall.orig_mmap = (mmap_ptr_t)dlsym(RTLD_NEXT, "mmap");
    if (opt_syscall.orig_mmap == NULL) {
        fprintf(stderr, "Error in dlsym(RTLD_NEXT,\"mmap\")\n");
        return SMDK_RET_INIT_MMAP_PTR_FAIL;
    } else {
        opt_syscall.is_initialized = true;
        return SMDK_RET_SUCCESS;
    }
}

inline bool is_cur_prio_exmem(int *prio) {
    *prio = get_current_prio();
    return (opt_smdk.prio[*prio] == mem_zone_exmem);
}

inline size_t malloc_usable_size_internal (mem_zone_t type, void *ptr) { /* added for redis */
    if (unlikely(!is_memtype_valid(type))) return 0;
    set_tsd_set_cur_mem_type(type);
	return je_malloc_usable_size(ptr);
}
