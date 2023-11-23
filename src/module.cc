//
// Created by victoryang00 on 11/9/23.
//

/** for thread creation and memory monitor */
#include "sock.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define CXLMEMSIM_EXPORT __attribute__((visibility("default")))
#define CXLMEMSIM_CONSTRUCTOR(n) __attribute__((constructor((n))))
#define CXLMEMSIM_CONSTRUCTOR_PRIORITY 102

typedef void *(*mmap_ptr_t)(void *, size_t, int, int, int, off_t);
typedef int (*munmap_ptr_t)(void *, size_t);
typedef void *(*malloc_ptr_t)(size_t);
typedef int (*calloc_ptr_t)(void *, size_t);
typedef void *(*realloc_ptr_t)(void *, size_t);
typedef int (*posix_memalign_ptr_t)(void **, size_t, size_t);
typedef void *(*aligned_alloc_ptr_t)(size_t, size_t);
typedef int (*free_ptr_t)(void *);
typedef int (*pthread_create_ptr_t)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
typedef int (*pthread_join_ptr_t)(pthread_t, void **);
typedef int (*pthread_detach_ptr_t)(pthread_t);
typedef size_t (*malloc_usable_size_ptr_t)(void *);
// typedef int (*mpi_send_t)(const void *buf, int count, MPI_Datatype datatype, int dest, int tag, MPI_Comm comm);

typedef struct cxlmemsim_param {
    int sock;
    struct sockaddr_un addr;
    mmap_ptr_t mmap;
    munmap_ptr_t munmap;
    malloc_ptr_t malloc;
    calloc_ptr_t calloc;
    realloc_ptr_t realloc;
    posix_memalign_ptr_t posix_memalign;
    aligned_alloc_ptr_t aligned_alloc;
    free_ptr_t free;
    pthread_create_ptr_t pthread_create;
    pthread_join_ptr_t pthread_join;
    pthread_detach_ptr_t pthread_detach;
    malloc_usable_size_ptr_t malloc_usable_size;
} cxlmemsim_param_t;

cxlmemsim_param_t param = {.sock = 0,
                           .addr = {},
                           .mmap = nullptr,
                           .munmap = nullptr,
                           .malloc = nullptr,
                           .free = nullptr,
                           .pthread_create = nullptr,
                           .pthread_join = nullptr,
                           .pthread_detach = nullptr};

inline void call_socket_with_int3() {
    const char *message = "hello";
    fprintf(stderr, "call_socket_with_int3\n");
    // sendback tid

    __asm__("int $0x3");
}

inline int init_mmap_ptr(void) {
    if (param.mmap == nullptr) {
        param.mmap = (mmap_ptr_t)dlsym(RTLD_NEXT, "mmap64");
        if (!param.mmap) {
            fprintf(stderr, "Error in dlsym(RTLD_NEXT,\"mmap\")\n");
            return -1;
        }
    }
    return 0;
}

CXLMEMSIM_EXPORT
void *malloc(size_t size) {
    call_socket_with_int3();
    fprintf(stderr, "malloc%ld\n", size);
    return param.malloc(size);
}

CXLMEMSIM_EXPORT
void *calloc(size_t num, size_t size) {
    call_socket_with_int3();
    if (param.mmap == nullptr) {
        return (void *)param.calloc;
    }

    return param.malloc(num * size);
}

CXLMEMSIM_EXPORT
void *realloc(void *ptr, size_t size) {
    call_socket_with_int3();
    return param.realloc(ptr, size);
}

CXLMEMSIM_EXPORT
int posix_memalign(void **memptr, size_t alignment, size_t size) {
    call_socket_with_int3();
    return param.posix_memalign(memptr, alignment, size);
}

CXLMEMSIM_EXPORT
void *aligned_alloc(size_t alignment, size_t size) {
    call_socket_with_int3();
    return param.aligned_alloc(alignment, size);
}

CXLMEMSIM_EXPORT
void free(void *ptr) {
    call_socket_with_int3();
    if (ptr == (void *)param.calloc) {
        return;
    }

    param.free(ptr);
}

CXLMEMSIM_EXPORT
void *mmap(void *start, size_t len, int prot, int flags, int fd, off_t off) {
    call_socket_with_int3();
    void *ret = NULL;
    int mmap_initialized = init_mmap_ptr();

    if (mmap_initialized != 0) {
        fprintf(stderr, "init_mmap_ptr() failed\n");
        return ret;
    }
    ret = param.mmap(start, len, prot, flags, fd, off);

    return ret;
}

CXLMEMSIM_EXPORT
void *mmap64(void *start, size_t len, int prot, int flags, int fd, off_t off) {
    call_socket_with_int3();
    return mmap(start, len, prot, flags, fd, off);
}

CXLMEMSIM_EXPORT
size_t malloc_usable_size(void *ptr) { /* added for redis */
    call_socket_with_int3();
    return param.malloc_usable_size(ptr);
}

CXLMEMSIM_CONSTRUCTOR(CXLMEMSIM_CONSTRUCTOR_PRIORITY) static void cxlmemsim_constructor() {
    // save the original impl of mmap

    init_mmap_ptr();
    param.munmap = (munmap_ptr_t)dlsym(RTLD_NEXT, "munmap");
    if (!param.munmap) {
        fprintf(stderr, "Error in dlsym(RTLD_NEXT,\"munmap\")\n");
        exit(-1);
    }
    param.malloc = (malloc_ptr_t)dlsym(RTLD_NEXT, "malloc");
    if (!param.malloc) {
        fprintf(stderr, "Error in dlsym(RTLD_NEXT,\"malloc\")\n");
        exit(-1);
    }
    param.free = (free_ptr_t)dlsym(RTLD_NEXT, "free");
    if (!param.free) {
        fprintf(stderr, "Error in dlsym(RTLD_NEXT,\"free\")\n");
        exit(-1);
    }
    param.calloc = (calloc_ptr_t)dlsym(RTLD_NEXT, "calloc");
    if (!param.calloc) {
        fprintf(stderr, "Error in dlsym(RTLD_NEXT,\"calloc\")\n");
        exit(-1);
    }
    param.realloc = (realloc_ptr_t)dlsym(RTLD_NEXT, "realloc");
    if (!param.realloc) {
        fprintf(stderr, "Error in dlsym(RTLD_NEXT,\"realloc\")\n");
        exit(-1);
    }
    param.pthread_create = (pthread_create_ptr_t)dlsym(RTLD_NEXT, "pthread_create");
    if (!param.pthread_create) {
        fprintf(stderr, "Error in dlsym(RTLD_NEXT,\"pthread_create\")\n");
        exit(-1);
    }

    param.pthread_detach = (pthread_detach_ptr_t)dlsym(RTLD_NEXT, "pthread_detach");
    if (!param.pthread_detach) {
        fprintf(stderr, "Error in dlsym(RTLD_NEXT,\"pthread_detach\")\n");
        exit(-1);
    }

    param.pthread_join = (pthread_join_ptr_t)dlsym(RTLD_NEXT, "pthread_join");
    if (!param.pthread_join) {
        fprintf(stderr, "Error in dlsym(RTLD_NEXT,\"pthread_join\")\n");
        exit(-1);
    }
    param.sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    /** register the original impl */
    struct sockaddr_un addr {};
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    fprintf(stderr, "start\n");
}

__attribute__((destructor)) static void cxlmemsim_destructor() { fprintf(stderr, "fini"); }
