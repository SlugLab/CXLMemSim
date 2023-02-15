#include <mutex>
#include <new>

#ifdef __cplusplus
extern "C" {
#endif

#include "core/include/internal/alloc.h"
#include "internal/cxlmalloc.h"

#ifdef __cplusplus
}
#endif

void *operator new(std::size_t size);
void *operator new[](std::size_t size);
void *operator new(std::size_t size, const std::nothrow_t &) noexcept;
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept;
void operator delete(void *ptr) noexcept;
void operator delete[](void *ptr) noexcept;
void operator delete(void *ptr, const std::nothrow_t &) noexcept;
void operator delete[](void *ptr, const std::nothrow_t &) noexcept;

/* NOTE: C++14's sized-delete operators is not supported for now. */

static void *
handleOOM(std::size_t size, bool nothrow) {
    void *ptr = nullptr;

    while (ptr == nullptr) {
        std::new_handler handler;
        // GCC-4.8 and clang 4.0 do not have std::get_new_handler.
        {
            static std::mutex mtx;
            std::lock_guard<std::mutex> lock(mtx);

            handler = std::set_new_handler(nullptr);
            std::set_new_handler(handler);
        }
        if (handler == nullptr)
            break;

        try {
            handler();
        } catch (const std::bad_alloc &) {
            break;
        }
        mem_zone_t type = get_cur_prioritized_memtype();
        ptr = s_malloc_internal(type, size, false);
    }

    if (ptr == nullptr && !nothrow)
        std::__throw_bad_alloc();
    return ptr;
}

template <bool IsNoExcept>
SMDK_INLINE
void *
newImpl(std::size_t size) noexcept(IsNoExcept) {
    mem_zone_t type = get_cur_prioritized_memtype();
    void *ret = s_malloc_internal(type, size, false);
    if (likely(ret)) {
        return ret;
    }
    return handleOOM(size, IsNoExcept);
}

void *
operator new(std::size_t size) {
    return newImpl<false>(size);
}

void *
operator new[](std::size_t size) {
    return newImpl<false>(size);
}

void *
operator new(std::size_t size, const std::nothrow_t &) noexcept {
    return newImpl<true>(size);
}

void *
operator new[](std::size_t size, const std::nothrow_t &) noexcept {
    return newImpl<true>(size);
}

void
operator delete(void *ptr) noexcept {
    return s_free_internal(ptr);
}

void
operator delete[](void *ptr) noexcept {
    return s_free_internal(ptr);
}

void
operator delete(void *ptr, const std::nothrow_t &) noexcept {
    return s_free_internal(ptr);
}

void operator delete[](void *ptr, const std::nothrow_t &) noexcept {
    return s_free_internal(ptr);
}
