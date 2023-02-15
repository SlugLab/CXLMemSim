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

#include "internal/opt_api.h"
#include "internal/config.h"
#include "jemalloc/jemalloc.h"
#include <sys/sysinfo.h>

smdk_config opt_smdk = {
    .use_exmem = true,
    .prio[0] = mem_zone_normal, /* prio high */
    .prio[1] = mem_zone_exmem, /* prio low */
    .exmem_zone_size = MAX_MEMZONE_LIMIT_MB, /* MB in size */
    .normal_zone_size = MAX_MEMZONE_LIMIT_MB, /* MB in size */
    .use_auto_arena_scaling = true, /* when enabled, create cpu*2 arenas overriding nr_normal_arena/nr_exmem_arena*/
    .nr_normal_arena = 1, /* the number of arena in arena pool */
    .nr_exmem_arena = 1, /* the number of arena in arena pool */
	.maxmemory_policy = 0, /* maxmemory_policy : oom, interleave, remain */
};

smdk_param smdk_info = {
    .current_prio = 0,
    .smdk_initialized = false,
    .get_target_arena = NULL,
};

arena_pool g_arena_pool[2];

SMDK_INLINE unsigned get_auto_scale_target_arena(mem_zone_t type){
    malloc_cpuid_t cpuid = malloc_getcpu();
    assert(cpuid >= 0);
    int pool_idx = (opt_smdk.prio[0] == type)? 0:1;
    arena_pool* pool = &g_arena_pool[pool_idx];

    return pool->arena_id[cpuid%pool->nr_arena];
}

SMDK_INLINE unsigned get_normal_target_arena(mem_zone_t type){
    int pool_idx = (opt_smdk.prio[0] == type)? 0:1;
    arena_pool* pool = &g_arena_pool[pool_idx];

    /* the value 'idx' would be changed during update_arena_pool -> max_memory_policy of cxlmalloc */
    return pool->arena_id[pool->arena_index%pool->nr_arena];
}

static int scale_arena_pool(){
    int nrcpu = sysconf(_SC_NPROCESSORS_ONLN);
    assert(nrcpu >0);

    int narenas = 0;
    if(opt_smdk.use_auto_arena_scaling){
        /* for SMP systems, create 2 normal/exmem arena per cpu by default */
        narenas = nrcpu << ARENA_AUTOSCALE_FACTOR;
        opt_smdk.nr_normal_arena = MIN(narenas, NR_ARENA_MAX);
        opt_smdk.nr_exmem_arena = MIN(narenas, NR_ARENA_MAX);
    }
    else{
        narenas = nrcpu << ARENA_SCALE_FACTOR;
        opt_smdk.nr_normal_arena = MIN(narenas, NR_ARENA_MAX);
        opt_smdk.nr_exmem_arena = MIN(narenas, NR_ARENA_MAX);
    }

    return narenas;
}

static int init_smdk_mutex(pthread_mutex_t* lock){
    if(!lock)
        return -1;

    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr) != 0) {
        return 1;
    }
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_DEFAULT);
    if (pthread_mutex_init(lock, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        return 1;
    }
    pthread_mutexattr_destroy(&attr);
    return 0;
}

static int init_smdk_rwlock(pthread_rwlock_t* lock){
    if(!lock)
        return -1;

    pthread_rwlockattr_t attr;

    if (pthread_rwlockattr_init(&attr) != 0) {
        return 1;
    }
    pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE);
    if (pthread_rwlock_init(lock, &attr) != 0) {
        pthread_rwlockattr_destroy(&attr);
        return 1;
    }
    pthread_rwlockattr_destroy(&attr);
    return 0;
}

static int init_arena_pool(){
    int i,j;
    size_t sz = sizeof(unsigned);
    if(opt_smdk.prio[0] == mem_zone_exmem){
        g_arena_pool[0].nr_arena = opt_smdk.nr_exmem_arena;
        g_arena_pool[0].zone_limit = opt_smdk.exmem_zone_size;

        g_arena_pool[1].nr_arena = opt_smdk.nr_normal_arena;
        g_arena_pool[1].zone_limit = opt_smdk.normal_zone_size;
    }
    else{
        g_arena_pool[0].nr_arena = opt_smdk.nr_normal_arena;
        g_arena_pool[0].zone_limit = opt_smdk.normal_zone_size;

        g_arena_pool[1].nr_arena = opt_smdk.nr_exmem_arena;
        g_arena_pool[1].zone_limit = opt_smdk.exmem_zone_size;
    }

    for(i=0;i<2;i++){
        for(j=0;j<g_arena_pool[i].nr_arena;j++){
            if(je_mallctl("arenas.create", (void *)&g_arena_pool[i].arena_id[j], &sz, NULL, 0)){
                fprintf(stderr,"arena_pool[%s] arena.create failure\n",str_priority(i));
                assert(false);
            }

            arena_t* arena = arena_get(TSDN_NULL,g_arena_pool[i].arena_id[j],false);
            if(opt_smdk.prio[i] == mem_zone_normal){ //prio[0] == normal, prio[1] == exmem by default
                arena_set_mmap_flag(arena, MAP_NORMAL_MEM);
            }
            else{
                arena_set_mmap_flag(arena, MAP_EXMEM);
            }
        }
        g_arena_pool[i].zone_allocated = 0;
        g_arena_pool[i].arena_index = 0;
        assert(!init_smdk_rwlock(&g_arena_pool[i].rwlock_zone_allocated));
        assert(!init_smdk_rwlock(&g_arena_pool[i].rwlock_arena_index));
        init_smdk_mutex(NULL);
    }
    return 0;
}

int init_smdk(void){
	if (smdk_info.smdk_initialized) return 0;

    if (opt_smdk.use_exmem == true) {
        scale_arena_pool();
        if(opt_smdk.use_auto_arena_scaling){
            smdk_info.get_target_arena = get_auto_scale_target_arena;
        }
        else {
            smdk_info.get_target_arena = get_normal_target_arena;
        }
        init_smdk_rwlock(&smdk_info.rwlock_current_prio);
        smdk_info.current_prio = 0;
        smdk_info.maxmemory_policy = opt_smdk.maxmemory_policy;

        init_arena_pool();

        smdk_info.smdk_initialized = true;

        init_system_mem_size();
        return SMDK_RET_SUCCESS;
    } else {
		return SMDK_RET_USE_EXMEM_FALSE;
	}
}

int init_smalloc(void) {
    smdk_init_helper("SMALLOC_CONF", true);
    return init_smdk();
}

inline mem_zone_t get_memtype_from_arenaidx(unsigned idx) {
	if (unlikely(idx == -1)) {
		return mem_zone_invalid;
	}
	if (idx < g_arena_pool[mem_zone_exmem].arena_id[0]) {
		return mem_zone_normal;
	} else {
		return mem_zone_exmem;
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

/*******************************************************************************
 * Metadata statistics functions
 ******************************************************************************/

void init_node_stats() {
    FILE* fs;
    fs = fopen("/proc/zoneinfo", "r");
    if (!fs) {
        fprintf(stderr, "get zoneinfo failure\n");
        return;
    }
    int normal_zone_node=0;
    int exmem_zone_node=0;
    char str[MAX_CHAR_LEN];
    char *zone;

    while(1){
        if(fgets(str,MAX_CHAR_LEN,fs) == NULL){
            break;
        }

        if(!strncmp(str,"Node",4)){
            strtok(str," ");
            strtok(NULL,",");
            strtok(NULL," ");
            zone=strtok(NULL," \n");
            if(!strcmp(zone,NAME_NORMAL_ZONE)){
                normal_zone_node++;
            }else if(!strcmp(zone,NAME_EXMEM_ZONE)){
                exmem_zone_node++;
            }
        }
    }
    fclose(fs);

    smdk_info.stats_per_node[mem_zone_normal].nr_node = normal_zone_node;
    smdk_info.stats_per_node[mem_zone_exmem].nr_node = exmem_zone_node;
    smdk_info.stats_per_node[mem_zone_normal].total \
        = (size_t *)malloc(sizeof(size_t) * normal_zone_node);
    smdk_info.stats_per_node[mem_zone_normal].available \
        = (size_t *)malloc(sizeof(size_t) * normal_zone_node);
    smdk_info.stats_per_node[mem_zone_exmem].total \
        = (size_t *)malloc(sizeof(size_t) * exmem_zone_node);
    smdk_info.stats_per_node[mem_zone_exmem].available \
        = (size_t *)malloc(sizeof(size_t) * exmem_zone_node);

}

void init_system_mem_size(void) { //set mem_stats 'total' during initialization
    init_node_stats();
    FILE* fs;
    fs = fopen("/proc/zoneinfo", "r");
    if (!fs) {
        fprintf(stderr, "get zoneinfo failure\n");
        return;
    }

    char str[MAX_CHAR_LEN];
    char *zone;
    char *tok;
    int node;
    long page_size = sysconf(_SC_PAGESIZE);

    size_t normal_zone_total = 0;
    size_t exmem_zone_total = 0;
    while(1){
        if(fgets(str,MAX_CHAR_LEN,fs)==NULL){
            break;
        }
        if(!strcmp(strtok(str," "),"Node")){
            node = atoi(strtok(NULL,","));
            strtok(NULL," ");
            zone = strtok(NULL," \n");
            if(!strcmp(zone,NAME_NORMAL_ZONE)){
                if(fgets(str,MAX_CHAR_LEN,fs)==NULL){
                    goto parsing_fail;
                }
                tok=strtok(str," ");
                while(strcmp(tok,"present")){
                    if(fgets(str,MAX_CHAR_LEN,fs)==NULL){
                        goto parsing_fail;
                    }
                    tok=strtok(str," ");
                }
                smdk_info.stats_per_node[mem_zone_normal].total[node] = atoi(strtok(NULL," "))*page_size;
                normal_zone_total+=smdk_info.stats_per_node[mem_zone_normal].total[node];
            }else if(!strcmp(zone,NAME_EXMEM_ZONE)){
                if(fgets(str,MAX_CHAR_LEN,fs)==NULL){
                    goto parsing_fail;
                }
                tok=strtok(str," ");
                while(strcmp(tok,"present")){
                    if(fgets(str,MAX_CHAR_LEN,fs)==NULL){
                        goto parsing_fail;
                    }
                    tok=strtok(str," ");
                }
                smdk_info.stats_per_node[mem_zone_exmem].total[node]=atoi(strtok(NULL," "))*page_size;
                exmem_zone_total+=smdk_info.stats_per_node[mem_zone_exmem].total[node];
            }
        }
    }

    fclose(fs);

    smdk_info.mem_stats_perzone[mem_zone_normal].total = normal_zone_total;
    smdk_info.mem_stats_perzone[mem_zone_exmem].total = exmem_zone_total;
    return;

parsing_fail:
    fclose(fs);
    fprintf(stderr, "parsing zoneinfo failure\n");
    return;
}

size_t get_mem_stats_total(mem_zone_t zone){ //metaAPI
    if (!smdk_info.smdk_initialized){
        init_smalloc();
    }
    return smdk_info.mem_stats_perzone[zone].total;
}

size_t get_mem_stats_used(mem_zone_t zone){ //metaAPI
    char buf[100];
    int pool_idx = zone; /*pool[0]: normal, pool[1]: exmem*/
    uint64_t epoch = 1;
    size_t sz = sizeof(epoch);
    je_mallctl("epoch", &epoch, &sz, &epoch, sz);

    size_t memsize_used=0;
    size_t allocated=0;
    for(int i=0;i<g_arena_pool[pool_idx].nr_arena;i++){
        snprintf(buf,100,"stats.arenas.%d.small.allocated",g_arena_pool[pool_idx].arena_id[i]);
        if(je_mallctl(buf, &allocated, &sz, NULL, 0) == 0){
            memsize_used+=allocated;
        }
        snprintf(buf,100,"stats.arenas.%d.large.allocated",g_arena_pool[pool_idx].arena_id[i]);
        if(je_mallctl(buf, &allocated, &sz, NULL, 0) == 0){
            memsize_used+=allocated;
        }
    }

    smdk_info.mem_stats_perzone[pool_idx].used = memsize_used;
    return memsize_used;
}

size_t get_mem_stats_available(mem_zone_t zone){ //metaAPI
    /*This function calculates available memory size based on '/proc/buddyinfo'*/
    if (!smdk_info.smdk_initialized){
        init_smalloc();
    }
    FILE* fs;
    char str[MAX_CHAR_LEN];
    char* zone_name;
    char* freepages;
    char* zone_name_target[2] = {NAME_NORMAL_ZONE, NAME_EXMEM_ZONE};
    size_t memsize_available_node=0;
    size_t memsize_available_total=0;
    long page_sz = sysconf(_SC_PAGESIZE);
    int node;
    int i;
    fs = fopen("/proc/buddyinfo", "r");
    if (!fs) {
        fprintf(stderr, "get buddyinfo failure\n");
        return 0;
    }
    while(1){
        i=0;
        if(fgets(str,MAX_CHAR_LEN,fs) == NULL){
            break;
        }
        str[strlen(str) -1]='\0';
        strtok(str," ");
        node = atoi(strtok(NULL,","));
        strtok(NULL," ");
        zone_name = strtok(NULL," ");
        if(!strncasecmp(zone_name, zone_name_target[zone], strlen(zone_name_target[zone]))){
            freepages = strtok(NULL," ");
            while(freepages!=NULL){
                memsize_available_node+=atoi(freepages)*(page_sz*(1<<i));
                i++;
                freepages = strtok(NULL," ");
            }
            smdk_info.stats_per_node[zone].available[node]=memsize_available_node;
            memsize_available_total+=memsize_available_node;
            memsize_available_node = 0;
        }
    }
    fclose(fs);
    smdk_info.mem_stats_perzone[zone].available = memsize_available_total;
    return memsize_available_total;
}

void mem_stats_print(void) {
    size_t mem_total_normal = get_mem_stats_total(mem_zone_normal);
    size_t mem_total_exmem = get_mem_stats_total(mem_zone_exmem);
    size_t mem_used_normal = get_mem_stats_used(mem_zone_normal);
    size_t mem_used_exmem = get_mem_stats_used(mem_zone_exmem);
    size_t mem_available_normal = get_mem_stats_available(mem_zone_normal);
    size_t mem_available_exmem = get_mem_stats_available(mem_zone_exmem);

    fprintf(stderr, "SMDK Memory allocation stats:\n");
    fprintf(stderr, "%8s %15s %15s %15s\n", "Type", "Total", "Used", "Available");
    fprintf(stderr, "%8s %15zu %15zu %15zu\n", "Normal",\
            mem_total_normal, mem_used_normal, mem_available_normal);
    fprintf(stderr, "%8s %15zu %15zu %15zu\n", "ExMem",\
            mem_total_exmem, mem_used_exmem, mem_available_exmem);
    return;
}

void stats_per_node_print(void) {
    int i;
    get_mem_stats_available(mem_zone_normal);
    get_mem_stats_available(mem_zone_exmem);

    fprintf(stderr, "SMDK Memory allocation stats per node:\n");
    fprintf(stderr, "%8s %6s %15s %15s\n", "Type", "Node", "Total", "Available");
    if(smdk_info.stats_per_node[mem_zone_normal].nr_node){
        for(i=0;i<smdk_info.stats_per_node[mem_zone_normal].nr_node; i++){
            fprintf(stderr, "%8s %6d %15zu %15zu\n", "Normal",\
                i, smdk_info.stats_per_node[mem_zone_normal].total[i],\
                smdk_info.stats_per_node[mem_zone_normal].available[i]);
        }
    }
    if(smdk_info.stats_per_node[mem_zone_exmem].nr_node){
        for(i=0;i<smdk_info.stats_per_node[mem_zone_exmem].nr_node; i++){
            fprintf(stderr, "%8s %6d %15zu %15zu\n", "ExMem",\
                i, smdk_info.stats_per_node[mem_zone_exmem].total[i],\
                smdk_info.stats_per_node[mem_zone_exmem].available[i]);
        }
    }
    return;
}
