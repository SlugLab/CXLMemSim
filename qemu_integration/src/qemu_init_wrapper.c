#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include "../include/qemu_cxl_memsim.h"

static void __attribute__((constructor)) cxlmemsim_constructor(void) {
    const char *host = getenv("CXL_MEMSIM_HOST");
    const char *port_str = getenv("CXL_MEMSIM_PORT");
    
    if (!host) host = "127.0.0.1";
    int port = port_str ? atoi(port_str) : 9999;
    
    fprintf(stderr, "Initializing CXLMemSim connection to %s:%d\n", host, port);
    
    if (cxlmemsim_init(host, port) < 0) {
        fprintf(stderr, "Warning: Failed to initialize CXLMemSim\n");
    }
}

// External kbd hook cleanup function
extern void cleanup_kbd_hook(void);

static void __attribute__((destructor)) cxlmemsim_destructor(void) {
    fprintf(stderr, "Cleaning up CXLMemSim connection\n");
    cxlmemsim_dump_hotness_stats();
    cxlmemsim_cleanup();
    cleanup_kbd_hook();
}