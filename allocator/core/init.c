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
#include <numa.h>

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
    .exmem_partition_range = {0, },
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
            assert(arena != NULL);
            if(opt_smdk.prio[i] == mem_zone_normal){ //prio[0] == normal, prio[1] == exmem by default
                arena_set_mmap_flag(arena, MAP_NORMAL);
            }else{
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

void init_cpu_node_config(){
    extern cpu_node_config_t je_cpu_node_config;
    je_cpu_node_config.nodemask = numa_no_nodes_ptr;
}


int init_smdk(void){
    if (smdk_info.smdk_initialized) return 0;

    init_cpu_node_config();

    if (opt_smdk.use_exmem == true) {
        scale_arena_pool();
        if(opt_smdk.use_auto_arena_scaling){
            smdk_info.get_target_arena = get_auto_scale_target_arena;
        }else{
            smdk_info.get_target_arena = get_normal_target_arena;
        }
        init_smdk_rwlock(&smdk_info.rwlock_current_prio);
        smdk_info.current_prio = 0;
        smdk_info.maxmemory_policy = opt_smdk.maxmemory_policy;

        init_arena_pool();
        smdk_info.smdk_initialized = true;
        return SMDK_RET_SUCCESS;
    } else {
        return SMDK_RET_USE_EXMEM_FALSE;
    }
}
