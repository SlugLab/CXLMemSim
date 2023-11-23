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

    size_t mbcount = 100;

    use_mmap = true;

    mmap_read = true;

    mmap_path = "./mmapfile";

    printf("allocating %ld MB\n", mbcount);
    uint8_t *p;
    int fd = open(mmap_path, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);

    if (fd < 0) {
        fprintf(stderr, "open() failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    if (mmap_write) {
        if (lseek(fd, mbcount * 1024ULL * 1024ULL, SEEK_CUR) == -1) {
            fprintf(stderr, "lseek() failed: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }

        // Make sure we have mbcount MB of valid file to mmap().
        if (write(fd, "trailer", sizeof("trailer")) <= 0) {
            fprintf(stderr, "write failed/short write\n");
            return EXIT_FAILURE;
        }
    }

    p = mmap(NULL, mbcount * 1024ULL * 1024ULL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap() failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

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

    if (use_mmap) {
        munmap(p, mbcount * 1024ULL * 1024ULL);
    }

    return EXIT_SUCCESS;
}