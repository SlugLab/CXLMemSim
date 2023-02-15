#ifndef INIT_H_
#define INIT_H_

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/hook.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/log.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/spin.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/ticker.h"
#include "jemalloc/internal/util.h"

#include <assert.h>
#include <limits.h>

/* config smdk */
#define UNLIMITED (-1)
#define MEMZONE_UNLIMITED SIZE_MAX
#define MAX_MEMZONE_LIMIT_MB    (1024 * 1024)    /* temporal limit, 1TB */
#define NR_ARENA_MAX 128
#define ARENA_AUTOSCALE_FACTOR 0
#define ARENA_SCALE_FACTOR 2

/* mmap flag */
#ifndef MAP_NORMAL
#define MAP_NORMAL 0x400000
#endif
#ifndef MAP_EXMEM
#define MAP_EXMEM 0x200000
#endif

/* utility constants */
#ifndef GB
#define GB (1<<30)
#endif

#ifndef MB
#define MB (1<<20)
#endif

#ifndef KB
#define KB (1024)
#endif

#define SMDK_EXPORT __attribute__((visibility("default")))
#define SMDK_CONSTRUCTOR(n) __attribute__((constructor ((n))))
#define SMALLOC_CONSTRUCTOR_PRIORITY 102 /* ~100: reserved for system, 101: reserved for cxlmalloc */
#define SMDK_INLINE static inline

#define NAME_NORMAL_ZONE "Normal"
#define NAME_EXMEM_ZONE "ExMem"
#define INVALID_MEM_SIZE (size_t)0

#define MAX_CHAR_LEN 200 /* metadata API */

typedef enum {
    mem_zone_normal=0,        /* point NORMAL_ZONE */
    mem_zone_exmem,            /* point EXMEM_ZONE */
    mem_zone_invalid        /* point invalid type */
} mem_zone_t;
extern mem_zone_t opt_mem_zone;

typedef struct { //metaAPI
    int nr_node;
    size_t *total;
    size_t *available;
} mem_stats_per_node;

typedef struct { //metaAPI
    size_t total;
    size_t used;
    size_t available;
} mem_stats_t;

typedef struct smdk_config{
    bool use_exmem;
    mem_zone_t prio[2];
    size_t exmem_zone_size;        /* byte in size */
    size_t normal_zone_size;    /* byte in size */
    bool use_auto_arena_scaling;
    uint32_t nr_normal_arena;        /* the number of normal arena in arena pool */
    uint32_t nr_exmem_arena;        /* the number of exmem arena in arena pool */
    int maxmemory_policy;            /* what will you do on 2nd pool goes to maxmemory */
    char exmem_partition_range[MAX_CHAR_LEN];
}smdk_config;
extern smdk_config opt_smdk;

typedef struct arena_pool{
    unsigned arena_id[NR_ARENA_MAX];    /* normal/exmem arena id */
    int nr_arena;                        /* the number of normal/exmem arena in arena pool */
    size_t zone_limit;                    /* maximum memory zone limit, MB */
    size_t zone_allocated;                /* allocated memory, MB */
    int arena_index;                    /* arena index, used only when use_auto_arena_scaling=false to traverse avaiable arena index(round-robin)*/
    pthread_rwlock_t rwlock_zone_allocated;
    pthread_rwlock_t rwlock_arena_index;
}arena_pool;
extern arena_pool g_arena_pool[2]; /* index 0 = high prio, 1 = low prio */
typedef unsigned (*get_target_arena_t)(mem_zone_t type);

typedef struct smdk_param{
    int current_prio;
    int maxmemory_policy;
    bool smdk_initialized;
    get_target_arena_t get_target_arena;
    pthread_rwlock_t rwlock_current_prio;
    mem_stats_t mem_stats_perzone[2]; //metaAPI
    mem_stats_per_node stats_per_node[2]; //metaAPI
    long long *node_alloc_stat_normal;
    long long *node_alloc_stat_exmem;
}smdk_param;
extern smdk_param smdk_info;

extern int init_smdk();

#endif /* INIT_H_ */
