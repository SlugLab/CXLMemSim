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

static int read_and_discard_line() {
    char *line = NULL;
    size_t linecap = 0;
    if (getline(&line, &linecap, stdin) == -1) {
        printf("getline() failed\n");
        return -1;
    }
    free(line);
    return 0;
}

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

    if (strcmp(argv[1], "malloc") == 0) {
        use_malloc = true;
        mbcount = atoi(argv[2]);
    } else {
        use_mmap = true;

        if (strcmp(argv[1], "mmap-read") == 0) {
            mmap_read = true;
        } else {
            mmap_write = true;
        }
        mbcount = atoi(argv[3]);
        mmap_path = argv[2];
    }

    printf("allocating %d MB\n", mbcount);
    uint8_t *p;
    if (use_malloc) {
        p = (uint8_t *)malloc(mbcount * 1024ULL * 1024ULL);

        if (p == NULL) {
            fprintf(stderr, "malloc()/mmap() failed: %s", strerror(errno));
            return EXIT_FAILURE;
        }
    } else if (use_mmap) {
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
    } else {
        fprintf(stderr, "bork\n");
        return EXIT_FAILURE;
    }

    printf("allocated - press enter to fill/read");
    if (read_and_discard_line() == -1) {
        return EXIT_FAILURE;
    }

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

    if (read_and_discard_line() == -1) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}