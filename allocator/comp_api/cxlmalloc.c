#include "internal/cxlmalloc.h"
#include "jemalloc/jemalloc.h"
#include <dlfcn.h>
#include <numa.h>

inline bool is_cxlmalloc_initialized(void) {
    return smdk_info.smdk_initialized;
}

inline bool is_use_exmem(void) {
    return opt_smdk.use_exmem;
}

void set_all_exmem_node_mask(struct bitmask* bmp) {
    FILE* fs;
    fs = fopen("/proc/buddyinfo", "r");
    if (!fs) {
        fprintf(stderr, "get zoneinfo failure\n");
        return;
    }
    int node_num;
    char str[MAX_CHAR_LEN];
    char *zone;

    while(1){
        if(fgets(str,MAX_CHAR_LEN,fs) == NULL){
            break;
        }

        if(!strncmp(str,"Node",4)){
            strtok(str," ");
            node_num=atoi(strtok(NULL,","));
            strtok(NULL," ");
            zone=strtok(NULL," \n");
            if(!strcmp(zone,NAME_EXMEM_ZONE)){
                numa_bitmask_setbit(bmp, node_num);
            }
        }
    }
    fclose(fs);
}

void set_exmem_partition_range_mask(void){
    extern cpu_node_config_t je_cpu_node_config;
    struct bitmask *bmp = numa_allocate_nodemask();

    if(strcmp(opt_smdk.exmem_partition_range, "")) {
        if (!strcmp(opt_smdk.exmem_partition_range, "all")) {
            /* all */
            set_all_exmem_node_mask(bmp);
        } else {
            /* node list */
            bmp = numa_parse_nodestring(opt_smdk.exmem_partition_range);
            if (bmp == numa_no_nodes_ptr || bmp == 0) {
                fprintf(stderr, "[Warning] Invalid value for \"exmem_partition_range\"=%s."
                        " This option will be ignored.\n", opt_smdk.exmem_partition_range);
                goto EXMEM_CONTROL_OFF;
            }
            /* filtering out the DDR nodes */
            struct bitmask *all_exmem_node_bmp = numa_allocate_nodemask();
            int num_nodes = numa_num_configured_nodes();
            set_all_exmem_node_mask(all_exmem_node_bmp);
            for (int i = 0 ; i < num_nodes; i++) {
                if (numa_bitmask_isbitset(bmp, i)) {
                    if (numa_bitmask_isbitset(all_exmem_node_bmp,i) == 0) {
                        fprintf(stderr, "[Warning] node %d is not ExMem node\n", i);
                        numa_bitmask_clearbit(bmp, i);
                    }
                }
            }
        }
        je_cpu_node_config.nodemask = bmp;
        return;
    }

EXMEM_CONTROL_OFF:
    je_cpu_node_config.nodemask = numa_no_nodes_ptr;
}

int init_cxlmalloc(void) {
    if (likely(is_cxlmalloc_initialized())) return SMDK_RET_SUCCESS;

    smdk_init_helper("CXLMALLOC_CONF", false);
    int ret = init_smdk();
    if (ret == SMDK_RET_USE_EXMEM_FALSE) {
        fprintf(stderr, "[Warning] \"use_exmem:false\". Jemalloc would be used instead.\n");
        return ret;
    }

    if (init_mmap_ptr() == SMDK_RET_INIT_MMAP_PTR_FAIL) exit(1);

    set_exmem_partition_range_mask();

    smdk_info.smdk_initialized = true;
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
            fprintf(stderr,"OOM - memory request exceeds limit\n");
            fprintf(stderr,"%s: terminated - maxmemory_policy=oom, current_prio=%d zone_allocated=%ld zone_limit=%ld\n", __FUNCTION__,smdk_info.current_prio, pool->zone_allocated, pool->zone_limit);
            exit(1);
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
    if (pool->zone_allocated > (pool->zone_limit)*MB) { /* zone_limit: MB */
        pthread_rwlock_unlock(&pool->rwlock_zone_allocated);
        if(get_current_prio() != prio){
            return 0;
        } else if (smdk_info.current_prio == 0) {
            pthread_rwlock_wrlock(&smdk_info.rwlock_current_prio);
            smdk_info.current_prio = 1;
            pthread_rwlock_unlock(&smdk_info.rwlock_current_prio);
            fprintf(stderr,"%s: CHANGE Priority 0->1\n", "update_arena_pool");
        } else {
            do_maxmemory_policy(pool);
        }
        if (opt_smdk.use_auto_arena_scaling == false) {
            pthread_rwlock_wrlock(&pool->rwlock_arena_index);
            pool->arena_index++;
            pthread_rwlock_unlock(&pool->rwlock_arena_index);
        }
    } else {
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
