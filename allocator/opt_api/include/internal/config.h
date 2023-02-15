#ifndef CONFIG_H_
#define CONFIG_H_

#define CONF_ERROR(msg, k, klen, v, vlen)               \
            fprintf(stderr, "%s: %.*s:%.*s\n", msg, (int)klen, k, (int)vlen, v);
#define CONF_MATCH(n)                           \
    (sizeof(n)-1 == klen && strncmp(n, k, klen) == 0)
#define CONF_MATCH_VALUE(n)                     \
    (sizeof(n)-1 == vlen && strncmp(n, v, vlen) == 0)
#define CONF_CHECK_MIN(um, min) ((um) < (min))
#define CONF_CHECK_MAX(um, max) ((um) > (max))
#define CONF_HANDLE_BOOL(o, n)                      \
            if (CONF_MATCH(n)) {                \
                if (CONF_MATCH_VALUE("true")) {     \
                    o = true;           \
                } else if (CONF_MATCH_VALUE("false")) { \
                    o = false;          \
                } else {                \
                    CONF_ERROR("Invalid conf value",\
                        k, klen, v, vlen);      \
                }                   \
            }
#define CONF_HANDLE_T_U(t, o, n, min, max, check_min, check_max, clip)  \
            if (CONF_MATCH(n)) {                \
                uintmax_t um;               \
                char *end;              \
                                    \
                set_errno(0);               \
                um = malloc_strtoumax(v, &end, 0);  \
                if (get_errno() != 0 || (uintptr_t)end -\
                    (uintptr_t)v != vlen) {     \
                    CONF_ERROR("Invalid conf value",\
                        k, klen, v, vlen);      \
                } else if (clip) {          \
                    if (check_min(um, (t)(min))) {  \
                        o = (t)(min);       \
                    } else if (         \
                        check_max(um, (t)(max))) {  \
                        o = (t)(max);       \
                    } else {            \
                        o = (t)um;      \
                    }               \
                } else {                \
                    if (check_min(um, (t)(min)) ||  \
                        check_max(um, (t)(max))) {  \
                        CONF_ERROR(     \
                            "Out-of-range " \
                            "conf value",   \
                            k, klen, v, vlen);  \
                    } else {            \
                        o = (t)um;      \
                    }               \
                }                   \
            }
#define CONF_HANDLE_UNSIGNED(o, n, min, max, check_min, check_max, clip)    \
            CONF_HANDLE_T_U(unsigned, o, n, min, max,   \
                check_min, check_max, clip)
#define CONF_HANDLE_SSIZE_T(o, n, min, max)             \
            if (CONF_MATCH(n)) {                \
                long l;                 \
                char *end;              \
                                    \
                set_errno(0);               \
                l = strtol(v, &end, 0);         \
                if (get_errno() != 0 || (uintptr_t)end -\
                    (uintptr_t)v != vlen) {     \
                    CONF_ERROR("Invalid conf value",\
                        k, klen, v, vlen);      \
                } else if (l < (ssize_t)(min) || l >    \
                    (ssize_t)(max)) {           \
                    CONF_ERROR(         \
                        "Out-of-range conf value",  \
                        k, klen, v, vlen);      \
                } else {                \
                    o = l;              \
                }                   \
            }

typedef enum {
    policy_oom,             /* priority : 1->0->error */
    policy_interleave,      /* priority : 1->0->1->0->... */
    policy_remain,          /* priority : 1->0->0->0->... */
} maxmemory_policy_t;

extern bool conf_next(char const **opts_p, char const **k_p, size_t *klen_p, 
		char const **v_p, size_t *vlen_p);
extern void smdk_init_helper(const char *envname, bool is_opt_api);
extern char *str_priority(mem_zone_t prio);
extern char *str_maxmemory_policy(maxmemory_policy_t policy);
extern void show_smdk_info(bool is_opt_api);

#define SMDK_RET_SUCCESS (0)
#define SMDK_RET_USE_EXMEM_FALSE (1)
#define SMDK_RET_INIT_MMAP_PTR_FAIL (2)

#endif /* CONFIG_H_ */
