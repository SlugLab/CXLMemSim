/* dax_writer.c - Write data to DAX device */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#define DEFAULT_DAX_DEVICE "/dev/dax0.0"
#define BUFFER_SIZE 4096

typedef struct {
    char magic[8];          // "DAXDATA"
    uint64_t timestamp;     // Write timestamp
    uint64_t data_size;     // Size of actual data
    uint64_t checksum;      // Simple checksum
    char data[];            // Flexible array for data
} dax_header_t;

// Simple checksum calculation
uint64_t calculate_checksum(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += bytes[i];
        sum = (sum << 1) | (sum >> 63); // Rotate left
    }
    return sum;
}

int main(int argc, char *argv[]) {
    const char *device = DEFAULT_DAX_DEVICE;
    const char *input_file = NULL;
    size_t offset = 0;
    int fd, input_fd = STDIN_FILENO;
    void *mapped_addr;
    struct stat sb;
    size_t map_size;
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            input_file = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            offset = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-d device] [-f input_file] [-o offset]\n", argv[0]);
            printf("  -d device      DAX device path (default: %s)\n", DEFAULT_DAX_DEVICE);
            printf("  -f input_file  Input file (default: stdin)\n");
            printf("  -o offset      Write offset in bytes (default: 0)\n");
            printf("  -h             Show this help\n");
            return 0;
        }
    }
    
    // Open input file if specified
    if (input_file) {
        input_fd = open(input_file, O_RDONLY);
        if (input_fd < 0) {
            fprintf(stderr, "Error opening input file %s: %s\n", 
                    input_file, strerror(errno));
            return EXIT_FAILURE;
        }
    }
    
    // Open DAX device
    printf("Opening DAX device: %s\n", device);
    fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Error opening device %s: %s\n", 
                device, strerror(errno));
        if (input_fd != STDIN_FILENO) close(input_fd);
        return EXIT_FAILURE;
    }
    
    // Get device size
    if (fstat(fd, &sb) < 0) {
        fprintf(stderr, "Error getting device stats: %s\n", strerror(errno));
        close(fd);
        if (input_fd != STDIN_FILENO) close(input_fd);
        return EXIT_FAILURE;
    }
    
    map_size = sb.st_size;
    printf("Device size: %zu bytes\n", map_size);
    
    if (offset >= map_size) {
        fprintf(stderr, "Error: Offset %zu exceeds device size %zu\n", 
                offset, map_size);
        close(fd);
        if (input_fd != STDIN_FILENO) close(input_fd);
        return EXIT_FAILURE;
    }
    
    // Memory map the device
    mapped_addr = mmap(NULL, map_size, PROT_READ | PROT_WRITE, 
                      MAP_SHARED, fd, 0);
    if (mapped_addr == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        close(fd);
        if (input_fd != STDIN_FILENO) close(input_fd);
        return EXIT_FAILURE;
    }
    
    printf("Successfully mapped at address: %p\n", mapped_addr);
    printf("Writing at offset: %zu\n", offset);
    
    // Prepare write location
    uint8_t *write_ptr = (uint8_t *)mapped_addr + offset;
    size_t available_space = map_size - offset - sizeof(dax_header_t);
    
    // Read input data
    uint8_t *data_buffer = malloc(BUFFER_SIZE);
    if (!data_buffer) {
        fprintf(stderr, "Memory allocation failed\n");
        munmap(mapped_addr, map_size);
        close(fd);
        if (input_fd != STDIN_FILENO) close(input_fd);
        return EXIT_FAILURE;
    }
    
    size_t total_read = 0;
    size_t capacity = BUFFER_SIZE;
    ssize_t bytes_read;
    
    // Read all input data
    while ((bytes_read = read(input_fd, data_buffer + total_read, 
                              capacity - total_read)) > 0) {
        total_read += bytes_read;
        
        // Expand buffer if needed
        if (total_read >= capacity) {
            capacity *= 2;
            uint8_t *new_buffer = realloc(data_buffer, capacity);
            if (!new_buffer) {
                fprintf(stderr, "Memory reallocation failed\n");
                free(data_buffer);
                munmap(mapped_addr, map_size);
                close(fd);
                if (input_fd != STDIN_FILENO) close(input_fd);
                return EXIT_FAILURE;
            }
            data_buffer = new_buffer;
        }
        
        // Check if we exceed available space
        if (total_read > available_space) {
            fprintf(stderr, "Error: Input data (%zu bytes) exceeds available space (%zu bytes)\n",
                    total_read, available_space);
            free(data_buffer);
            munmap(mapped_addr, map_size);
            close(fd);
            if (input_fd != STDIN_FILENO) close(input_fd);
            return EXIT_FAILURE;
        }
    }
    
    if (bytes_read < 0) {
        fprintf(stderr, "Error reading input: %s\n", strerror(errno));
        free(data_buffer);
        munmap(mapped_addr, map_size);
        close(fd);
        if (input_fd != STDIN_FILENO) close(input_fd);
        return EXIT_FAILURE;
    }
    
    // Prepare and write header
    dax_header_t *header = (dax_header_t *)write_ptr;
    strcpy(header->magic, "DAXDATA");
    header->timestamp = time(NULL);
    header->data_size = total_read;
    header->checksum = calculate_checksum(data_buffer, total_read);
    
    // Write data
    memcpy(header->data, data_buffer, total_read);
    
    printf("Wrote %zu bytes of data\n", total_read);
    printf("Timestamp: %lu\n", header->timestamp);
    printf("Checksum: 0x%lx\n", header->checksum);
    
    // Ensure persistence
    if (msync(write_ptr, sizeof(dax_header_t) + total_read, MS_SYNC) < 0) {
        fprintf(stderr, "Warning: msync failed: %s\n", strerror(errno));
    }
    
    // Cleanup
    free(data_buffer);
    if (input_fd != STDIN_FILENO) close(input_fd);
    munmap(mapped_addr, map_size);
    close(fd);
    
    printf("Write completed successfully\n");
    return EXIT_SUCCESS;
}