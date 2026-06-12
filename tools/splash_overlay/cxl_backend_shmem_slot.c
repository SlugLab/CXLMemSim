/*
 * Local build overlay for Splash's SHMEM backend.
 *
 * The upstream backend chooses a request slot with pthread_self() % num_slots.
 * That is fine for a long-lived process, but these benchmark harnesses launch
 * many short-lived single-threaded children and can repeatedly reuse slot 0.
 * Defining CXL_SHMEM_SLOT lets the harness pin each child to a deterministic
 * clean slot without editing the Splash checkout.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>

static pthread_t cxlmemsim_splash_slot_pthread_self(void) {
    const char *slot = getenv("CXL_SHMEM_SLOT");
    if (slot && *slot) {
        char *end = NULL;
        unsigned long value = strtoul(slot, &end, 0);
        if (end && *end == '\0') {
            return (pthread_t)value;
        }
    }
    return pthread_self();
}

#ifndef SPLASH_CXL_BACKEND_SHMEM_SOURCE
#define SPLASH_CXL_BACKEND_SHMEM_SOURCE "/root/splash/src/libpgas/src/cxl_backend_shmem.c"
#endif

#define pthread_self cxlmemsim_splash_slot_pthread_self
#include SPLASH_CXL_BACKEND_SHMEM_SOURCE
#undef pthread_self
