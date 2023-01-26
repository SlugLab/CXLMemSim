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

    if ((argc != 3 && argc != 4) ||
        (strcmp(argv[1], "malloc") != 0 && strcmp(argv[1], "mmap-read") != 0 && strcmp(argv[1], "mmap-write") != 0)) {
        fprintf(stderr, "usage: alloc <malloc|mmap-write <file>|mmap-read <file>> <amount-in-mb>\n");
        fprintf(stderr, "example: ./alloc malloc 100\n");
        fprintf(stderr, "example: ./alloc mmap-write bigfile 100\n");
        fprintf(stderr, "example: ./alloc mmap-read bigfile 100\n");
        fprintf(stderr, "notes:\n");
        fprintf(stderr, "  mmap-read requires a previous invocation of mmap-write\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "  WARNING: arguments are not properly validated\n");
        return EXIT_FAILURE;
    }

    size_t mbcount;

    if (strcmp(argv[1], "sbrk") == 0) {
        mbcount = atoi(argv[2]);
    }

    printf("allocating %ld MB\n", mbcount);
    uint8_t *p;
    p = (uint8_t *)malloc(mbcount * 1024ULL * 1024ULL);

    if (p == NULL) {
        fprintf(stderr, "malloc()/mmap() failed: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("allocated - press enter to fill/read");

    if (mmap_read) {
        printf("reading");
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