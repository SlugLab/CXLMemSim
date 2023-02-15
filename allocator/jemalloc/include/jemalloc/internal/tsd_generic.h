#ifdef JEMALLOC_INTERNAL_TSD_GENERIC_H
#error This file should be included only once, by tsd.h.
#endif
#define JEMALLOC_INTERNAL_TSD_GENERIC_H

typedef struct tsd_init_block_s tsd_init_block_t;
struct tsd_init_block_s {
    ql_elm(tsd_init_block_t) link;
    pthread_t thread;
    void *data;
};

/* Defined in tsd.c, to allow the mutex headers to have tsd dependencies. */
typedef struct tsd_init_head_s tsd_init_head_t;

typedef struct {
    bool initialized;
    tsd_t val[2];
    int cur_type;
    bool mem_policy_enabled;
} tsd_wrapper_t;

void *tsd_init_check_recursion(tsd_init_head_t *head,
    tsd_init_block_t *block);
void tsd_init_finish(tsd_init_head_t *head, tsd_init_block_t *block);

extern pthread_key_t tsd_tsd;
extern tsd_init_head_t tsd_init_head;
extern tsd_wrapper_t tsd_boot_wrapper;
extern bool tsd_booted;

/* Initialization/cleanup. */
JEMALLOC_ALWAYS_INLINE void
tsd_cleanup_wrapper(void *arg) {
    tsd_wrapper_t *wrapper = (tsd_wrapper_t *)arg;

    if (wrapper->initialized) {
        wrapper->initialized = false;
        tsd_cleanup(&wrapper->val[0]);
        tsd_cleanup(&wrapper->val[1]);

        if (wrapper->initialized) {
            /* Trigger another cleanup round. */
            if (pthread_setspecific(tsd_tsd, (void *)wrapper) != 0)
            {
                malloc_write("<jemalloc>: Error setting TSD\n");
                if (opt_abort) {
                    abort();
                }
            }
            return;
        }
    }
    malloc_tsd_dalloc(wrapper);
}

JEMALLOC_ALWAYS_INLINE void
tsd_wrapper_set(tsd_wrapper_t *wrapper) {
    if (pthread_setspecific(tsd_tsd, (void *)wrapper) != 0) {
        malloc_write("<jemalloc>: Error setting TSD\n");
        abort();
    }
}

JEMALLOC_ALWAYS_INLINE tsd_wrapper_t *
tsd_wrapper_get(bool init) {
    tsd_wrapper_t *wrapper = (tsd_wrapper_t *)pthread_getspecific(tsd_tsd);

    if (init && unlikely(wrapper == NULL)) {
        tsd_init_block_t block;
        wrapper = (tsd_wrapper_t *)
            tsd_init_check_recursion(&tsd_init_head, &block);
        if (wrapper) {
            return wrapper;
        }
        wrapper = (tsd_wrapper_t *)
            malloc_tsd_malloc(sizeof(tsd_wrapper_t));
        block.data = (void *)wrapper;
        if (wrapper == NULL) {
            malloc_write("<jemalloc>: Error allocating TSD\n");
            abort();
        } else {
            wrapper->initialized = false;
      JEMALLOC_DIAGNOSTIC_PUSH
      JEMALLOC_DIAGNOSTIC_IGNORE_MISSING_STRUCT_FIELD_INITIALIZERS
            tsd_t initializer = TSD_INITIALIZER;
      JEMALLOC_DIAGNOSTIC_POP
            wrapper->val[0] = initializer;
            wrapper->val[0].memtype = MEM_ZONE_NORMAL; /*mem_zone_normal*/
            wrapper->val[1] = initializer;
            wrapper->val[1].memtype = MEM_ZONE_EXMEM; /*mem_zone_exmem*/
            wrapper->cur_type = MEM_ZONE_NORMAL;
            wrapper->mem_policy_enabled = false;
        }
        tsd_wrapper_set(wrapper);
        tsd_init_finish(&tsd_init_head, &block);
    }
    return wrapper;
}

JEMALLOC_ALWAYS_INLINE void
set_tsd_set_cur_mem_type(int type) {
    tsd_wrapper_t* wrapper = tsd_wrapper_get(true);
    assert(wrapper);
    wrapper->cur_type = type;
}

JEMALLOC_ALWAYS_INLINE void
change_tsd_set_cur_mem_type(void) {
    tsd_wrapper_t* wrapper = tsd_wrapper_get(true);
    assert(wrapper);
    wrapper->cur_type = !wrapper->cur_type;
}

JEMALLOC_ALWAYS_INLINE bool
tsd_boot0(void) {
    if (pthread_key_create(&tsd_tsd, tsd_cleanup_wrapper) != 0) {
        return true;
    }
    tsd_wrapper_set(&tsd_boot_wrapper);
    tsd_booted = true;
    return false;
}

JEMALLOC_ALWAYS_INLINE void
tsd_boot1(void) {
    tsd_wrapper_t *wrapper;
    wrapper = (tsd_wrapper_t *)malloc_tsd_malloc(sizeof(tsd_wrapper_t));
    if (wrapper == NULL) {
        malloc_write("<jemalloc>: Error allocating TSD\n");
        abort();
    }
    tsd_boot_wrapper.initialized = false;
    tsd_cleanup(&tsd_boot_wrapper.val[0]);
    tsd_cleanup(&tsd_boot_wrapper.val[1]);
    wrapper->initialized = false;
JEMALLOC_DIAGNOSTIC_PUSH
JEMALLOC_DIAGNOSTIC_IGNORE_MISSING_STRUCT_FIELD_INITIALIZERS
    tsd_t initializer = TSD_INITIALIZER;
JEMALLOC_DIAGNOSTIC_POP
    wrapper->val[0] = initializer;
    wrapper->val[0].memtype = MEM_ZONE_NORMAL;
    wrapper->val[1] = initializer;
    wrapper->val[1].memtype = MEM_ZONE_EXMEM;
    wrapper->cur_type = MEM_ZONE_NORMAL;
    wrapper->mem_policy_enabled = false;
    tsd_wrapper_set(wrapper);
}

JEMALLOC_ALWAYS_INLINE bool
tsd_boot(void) {
    if (tsd_boot0()) {
        return true;
    }
    tsd_boot1();
    return false;
}

JEMALLOC_ALWAYS_INLINE bool
tsd_booted_get(void) {
    return tsd_booted;
}

JEMALLOC_ALWAYS_INLINE bool
tsd_get_allocates(void) {
    return true;
}

JEMALLOC_ALWAYS_INLINE tsd_t *
tsd_get_with_type(int type) {
    tsd_wrapper_t *wrapper = tsd_wrapper_get(false);
    if (tsd_get_allocates() && wrapper == NULL) {
        return NULL;
    }
    return &(wrapper->val[type]);
}

/* Get/set. */
JEMALLOC_ALWAYS_INLINE tsd_t *
tsd_get(bool init) {
    tsd_wrapper_t *wrapper;
    assert(tsd_booted);
    wrapper = tsd_wrapper_get(init);
    if (tsd_get_allocates() && !init && wrapper == NULL) {
        return NULL;
    }
    return &(wrapper->val[wrapper->cur_type]);
}

JEMALLOC_ALWAYS_INLINE void
tsd_set(tsd_t *val) {
    tsd_wrapper_t *wrapper;
    assert(tsd_booted);
    wrapper = tsd_wrapper_get(true);
    if (likely(&(wrapper->val[wrapper->cur_type]) != val)) {
        wrapper->val[wrapper->cur_type] = *(val);
    }
    wrapper->initialized = true;
}

JEMALLOC_ALWAYS_INLINE void
tsd_set_mem_policy_info(bool policy) {
    tsd_wrapper_t *wrapper;
    assert(tsd_booted);
    wrapper = tsd_wrapper_get(true);
    assert(wrapper != NULL);
    wrapper->mem_policy_enabled = policy;
}

JEMALLOC_ALWAYS_INLINE bool
tsd_is_mem_policy_enabled(bool init) {
    tsd_wrapper_t *wrapper;
    assert(tsd_booted);
    wrapper = tsd_wrapper_get(init);
    if (tsd_get_allocates() && !init && wrapper == NULL) {
        return -1;
    }
    return wrapper->mem_policy_enabled;
}

