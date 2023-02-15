#include "internal/init.h"
#include "internal/config.h"

bool is_number(char c) {
    return ((c-'0') >= 0 && (c-'0')<=9)? true : false;
}

bool conf_next(char const **opts_p, char const **k_p, size_t *klen_p,
    char const **v_p, size_t *vlen_p) {
    bool accept;
    const char *opts = *opts_p;

    *k_p = opts;
    for (accept = false; !accept;) {
        switch (*opts) {
        case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
        case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
        case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
        case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
        case 'Y': case 'Z':
        case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
        case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
        case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
        case 's': case 't': case 'u': case 'v': case 'w': case 'x':
        case 'y': case 'z':
        case '0': case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
        case '_':
            opts++;
            break;
        case ':':
            opts++;
            *klen_p = (uintptr_t)opts - 1 - (uintptr_t)*k_p;
            *v_p = opts;
            accept = true;
            break;
        case '\0':
            if (opts != *opts_p) {
                fprintf(stderr, "Conf string ends with key\n");
            }
            return true;
        default:
            fprintf(stderr, "Malformed conf string\n");
            return true;
        }
    }

    for (accept = false; !accept;) {
        switch (*opts) {
        case ',':
            opts++;
            if (!strncmp(*k_p, "exmem_partition_range", strlen("exmem_partition_range")) && is_number(*opts)) {
                /* to parse node mask values linked by comma e.g. 1,2,3 */
                break;
            }
            if (*opts == '\0') {
                fprintf(stderr, "Conf string ends with comma\n");
            }
            *vlen_p = (uintptr_t)opts - 1 - (uintptr_t)*v_p;
            accept = true;
            break;
        case '\0':
            *vlen_p = (uintptr_t)opts - (uintptr_t)*v_p;
            accept = true;
            break;
        default:
            opts++;
            break;
        }
    }

    *opts_p = opts;
    return false;
}

void smdk_init_helper(const char *envname, bool is_opt_api) {
    const char *opts = getenv(envname);
    const char *k, *v;
    size_t klen, vlen;

    if (smdk_info.smdk_initialized) return;

    if (opts != NULL) {
        while (*opts != '\0' && !conf_next(&opts, &k, &klen, &v, &vlen)) {
            if (!is_opt_api) {
                /* configs below are not used for optimization API libs */
                CONF_HANDLE_BOOL(opt_smdk.use_exmem,"use_exmem")
                CONF_HANDLE_SSIZE_T(opt_smdk.exmem_zone_size, "exmem_zone_size", 1, MAX_MEMZONE_LIMIT_MB)
                CONF_HANDLE_SSIZE_T(opt_smdk.normal_zone_size, "normal_zone_size", 1, MAX_MEMZONE_LIMIT_MB)
                CONF_HANDLE_UNSIGNED(opt_smdk.nr_normal_arena, "nr_normal_arena", 1, NR_ARENA_MAX,
                        CONF_CHECK_MIN, CONF_CHECK_MAX, false)
                CONF_HANDLE_UNSIGNED(opt_smdk.nr_exmem_arena, "nr_exmem_arena", 1, NR_ARENA_MAX,
                        CONF_CHECK_MIN, CONF_CHECK_MAX, false)

                if(CONF_MATCH("maxmemory_policy")){
                    if(CONF_MATCH_VALUE("oom")){
                        opt_smdk.maxmemory_policy = policy_oom;
                    }
                    else if(CONF_MATCH_VALUE("interleave")){
                        opt_smdk.maxmemory_policy = policy_interleave;
                    }
                    else if(CONF_MATCH_VALUE("remain")){
                        opt_smdk.maxmemory_policy = policy_remain;
                    }
                    else{
                        opt_smdk.maxmemory_policy = policy_oom;
                    }
                }

                if(CONF_MATCH("priority")){
                    if(CONF_MATCH_VALUE("exmem")){
                        opt_smdk.prio[0] = mem_zone_exmem;
                        opt_smdk.prio[1] = mem_zone_normal;
                    }
                    else if(CONF_MATCH_VALUE("normal")){
                        opt_smdk.prio[0] = mem_zone_normal;
                        opt_smdk.prio[1] = mem_zone_exmem;
                    }
                    else{
                        opt_smdk.prio[0] = mem_zone_normal;
                        opt_smdk.prio[1] = mem_zone_exmem;
                    }
                }

                CONF_HANDLE_CHAR_P(opt_smdk.exmem_partition_range,"exmem_partition_range", "")
            }
            CONF_HANDLE_BOOL(opt_smdk.use_auto_arena_scaling,"use_auto_arena_scaling")
        }
    }
}

char *str_priority(mem_zone_t prio){
    return (prio == mem_zone_normal) ? "normal" : "exmem";
}

char *str_maxmemory_policy(maxmemory_policy_t policy){
    switch(policy){
        case policy_oom: return "oom";break;
        case policy_interleave: return "interleave";break;
        case policy_remain: return "remain";break;
        default: return "invalid policy";
    }
}

void show_smdk_info(bool is_opt_api) {
    int i,j;
    fprintf(stderr,"use_exmem = %d\n", opt_smdk.use_exmem);
    fprintf(stderr,"use_auto_arena_scaling = %d\n", opt_smdk.use_auto_arena_scaling);
    for(i=0;i<2;i++){
        fprintf(stderr,"g_arena_pool[%s].nr_arena=%d limit=%ld\n\t[",
        str_priority(opt_smdk.prio[i]), g_arena_pool[i].nr_arena, g_arena_pool[i].zone_limit);
        for(j=0;j<g_arena_pool[i].nr_arena;j++){
            bool detail = false;
            if(detail){
                arena_t* arena = arena_get(TSDN_NULL,g_arena_pool[i].arena_id[j],false);
                fprintf(stderr,"%d(0x%08x), ",g_arena_pool[i].arena_id[j],arena_get_mmap_flag(arena));
            }
            else{
                fprintf(stderr,"%d, ",g_arena_pool[i].arena_id[j]);
            }
            if(!((i+1)%4)){
                fprintf(stderr,"\n\t");
            }
        }
        fprintf(stderr,"]\n");
    }
    if (!is_opt_api) {
        fprintf(stderr,"prio = [%s->%s]\n", str_priority(opt_smdk.prio[0]), str_priority(opt_smdk.prio[1]));
        if(opt_smdk.normal_zone_size==MEMZONE_UNLIMITED){
            fprintf(stderr,"normal_zone_size = unlimited\n");
        } else {
            fprintf(stderr,"normal_zone_size = %lu MB\n", opt_smdk.normal_zone_size);
        }
        if(opt_smdk.exmem_zone_size==MEMZONE_UNLIMITED){
            fprintf(stderr,"exmem_zone_size = unlimited\n");
        } else {
            fprintf(stderr,"exmem_zone_size = %lu MB\n",opt_smdk.exmem_zone_size);
        }
        fprintf(stderr,"maxmemory_policy = %s\n", str_maxmemory_policy(opt_smdk.maxmemory_policy));
        fprintf(stderr,"exmem_partition_range = %s\n", opt_smdk.exmem_partition_range);
    }
}
