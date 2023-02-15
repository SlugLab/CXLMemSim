#ifdef JEMALLOC_INTERNAL_TSD_TLS_H
#error This file should be included only once, by tsd.h.
#endif
#define JEMALLOC_INTERNAL_TSD_TLS_H

#define JEMALLOC_TSD_TYPE_ATTR(type) __thread type JEMALLOC_TLS_MODEL

typedef struct {
    tsd_t val[2];
    int cur_type;
    bool mem_policy_enabled;
} tsd_set_t;
extern JEMALLOC_TSD_TYPE_ATTR(tsd_set_t) tsd_set_tls;
extern pthread_key_t tsd_tsd;
extern bool tsd_booted;

/* Initialization/cleanup. */
JEMALLOC_ALWAYS_INLINE void
tsd_set_cleanup(void *arg) {
    tsd_set_t *tsd_set = (tsd_set_t *)arg;
    tsd_cleanup(&tsd_set->val[0]);
    tsd_cleanup(&tsd_set->val[1]);
}

JEMALLOC_ALWAYS_INLINE bool
tsd_boot0(void) {
    if (pthread_key_create(&tsd_tsd, &tsd_set_cleanup) != 0) {
        return true;
    }
    tsd_booted = true;
    return false;
}

JEMALLOC_ALWAYS_INLINE void
tsd_boot1(void) {
    /* Do nothing. */
}

JEMALLOC_ALWAYS_INLINE bool
tsd_boot(void) {
    return tsd_boot0();
}

JEMALLOC_ALWAYS_INLINE bool
tsd_booted_get(void) {
    return tsd_booted;
}

JEMALLOC_ALWAYS_INLINE bool
tsd_get_allocates(void) {
    return false;
}

/* Get/set. */
JEMALLOC_ALWAYS_INLINE void
set_tsd_set_cur_mem_type(int type) {
    tsd_set_tls.cur_type = type;
}

JEMALLOC_ALWAYS_INLINE void
change_tsd_set_cur_mem_type(void) {
    tsd_set_tls.cur_type = !(tsd_set_tls.cur_type);
}

JEMALLOC_ALWAYS_INLINE tsd_t *
tsd_get_with_type(int type){
    return &(tsd_set_tls.val[type]);
}

JEMALLOC_ALWAYS_INLINE tsd_t *
tsd_get(bool init) {
    return &(tsd_set_tls.val[tsd_set_tls.cur_type]);
}

JEMALLOC_ALWAYS_INLINE void
tsd_set(tsd_t *val) {
    assert(tsd_booted);
    if (likely(&(tsd_set_tls.val[tsd_set_tls.cur_type]) != val)) {
        tsd_set_tls.val[tsd_set_tls.cur_type] = (*val);
    }
    if (pthread_setspecific(tsd_tsd, (void *)(&tsd_set_tls)) != 0) {
        malloc_write("<jemalloc>: Error setting tsd.\n");
        if (opt_abort) {
            abort();
        }
    }
}

JEMALLOC_ALWAYS_INLINE bool
tsd_is_mem_policy_enabled(bool init) {
    return tsd_set_tls.mem_policy_enabled;
}

JEMALLOC_ALWAYS_INLINE void
tsd_set_mem_policy_info(bool policy) {
    tsd_set_tls.mem_policy_enabled = policy;
}
