// https://github.com/scode/alloctest
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, const char *const *argv) {
    bool use_malloc = false;
    bool use_mmap = false;
    bool mmap_read = false;
    bool mmap_write = false;
    const char *mmap_path;
    for (int i = 0; i < argc; i++) {
        printf("argv[%d] = %s\n", i, argv[i]);
    }

    use_malloc = true;

    size_t mbcount = 1000;

    mmap_path = "./mmapfile";

    printf("allocating %ld MB\n", mbcount);
    uint8_t *p;
    p = (uint8_t *)malloc(mbcount * 1024ULL * 1024ULL);

    if (p == NULL) {
        fprintf(stderr, "malloc()/mmap() failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("allocated - press enter to fill/read\n");

    if (mmap_read) {
        printf("reading\n");
        int sum = 0; /* make read not a noop just in case the compiler wants to be funny */
        for (size_t i = 0; i < mbcount * 1024ULL * 1024ULL; i++) {
            sum += (int)p[i];
        }
    } else {
        printf("filling\n");
        for (size_t i = 0; i < mbcount * 1024ULL * 1024ULL; i++) {
            p[i] = 'w';
        }
    }

    return EXIT_SUCCESS;
}