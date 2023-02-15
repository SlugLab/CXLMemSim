#include <assert.h>
#include <string.h>
#include "smdk_opt_api.h"

using std::string;

class SmdkAllocator
{
public:
    static SmdkAllocator& get_instance()
    {
        static SmdkAllocator s;
        return s;
    }
    void *malloc(smdk_memtype_t type, size_t size)
    {
        return s_malloc(type, size);
    }
    void *calloc(smdk_memtype_t type, size_t num, size_t size)
    {
        return s_calloc(type, num, size);
    }
    void *realloc(smdk_memtype_t type, void *ptr, size_t size)
    {
        return s_realloc(type, ptr, size);
    }
    int posix_memalign(smdk_memtype_t type, void **memptr, size_t alignment, size_t size)
    {
        return s_posix_memalign(type, memptr, alignment, size);
    }
    void free(void *ptr)
    {
        return s_free(ptr);
    }
    void free(smdk_memtype_t type, void *ptr)
    {
        return s_free_type(type, ptr);
    }
    size_t get_memsize_total(smdk_memtype_t type)
    {
        return s_get_memsize_total(type);
    }
    size_t get_memsize_used(smdk_memtype_t type)
    {
        return s_get_memsize_used(type);
    }
    size_t get_memsize_available(smdk_memtype_t type)
    {
        return s_get_memsize_available(type);
    }
    void stats_print(char unit)
    {
        s_stats_print(unit);
    }
    void stats_node_print(char unit)
    {
        s_stats_node_print(unit);
    }
    void enable_node_interleave(string nodes)
    {
        s_enable_node_interleave((char *)nodes.c_str());
    }
    void disable_node_interleave(void)
    {
        s_disable_node_interleave();
    }
    void *malloc_node(smdk_memtype_t type, size_t size, string nodes)
    {
        return s_malloc_node(type, size, (char *)nodes.c_str());
    }
    void free_node(smdk_memtype_t type, void *mem, size_t size)
    {
        return s_free_node(type, mem, size);
    }
private:
    SmdkAllocator() = default;
    SmdkAllocator(const SmdkAllocator& ref) = delete;
    SmdkAllocator& operator=(const SmdkAllocator& ref) = delete;
    ~SmdkAllocator() = default;
};
