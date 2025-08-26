/* dax_reader.c - Read data from DAX device */
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
#include <ctype.h>

#define DEFAULT_DAX_DEVICE "/dev/dax0.0"
#define DISPLAY_WIDTH 16

typedef struct {
    char magic[8];          // "DAXDATA"
    uint64_t timestamp;     // Write timestamp
    uint64_t data_size;     // Size of actual data
    uint64_t checksum;      // Simple checksum
    char data[];            // Flexible array for data
} dax_header_t;

// Simple checksum calculation (must match writer)
uint64_t calculate_checksum(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += bytes[i];
        sum = (sum << 1) | (sum >> 63); // Rotate left
    }
    return sum;
}

// Hexdump function for raw display
void hexdump(const void *addr, size_t len, size_t offset) {
    const unsigned char *bytes = (const unsigned char *)addr;
    size_t i, j;
    
    for (i = 0; i < len; i += DISPLAY_WIDTH) {
        // Print offset
        printf("%08zx  ", offset + i);
        
        // Print hex bytes
        for (j = 0; j < DISPLAY_WIDTH; j++) {
            if (i + j < len) {
                printf("%02x ", bytes[i + j]);
            } else {
                printf("   ");
            }
            if (j == 7) printf(" ");
        }
        
        printf(" |");
        
        // Print ASCII representation
        for (j = 0; j < DISPLAY_WIDTH && i + j < len; j++) {
            if (isprint(bytes[i + j])) {
                printf("%c", bytes[i + j]);
            } else {
                printf(".");
            }
        }
        
        printf("|\n");
    }
}

int main(int argc, char *argv[]) {
    const char *device = DEFAULT_DAX_DEVICE;
    const char *output_file = NULL;
    size_t offset = 0;
    size_t length = 0;
    int raw_mode = 0;
    int verify_checksum = 1;
    int fd, output_fd = STDOUT_FILENO;
    void *mapped_addr;
    struct stat sb;
    size_t map_size;
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            device = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            offset = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            length = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-r") == 0) {
            raw_mode = 1;
        } else if (strcmp(argv[i], "-n") == 0) {
            verify_checksum = 0;
        } else if (strcmp(argv[i], "-x") == 0) {
            raw_mode = 2; // Hexdump mode
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-d device] [-o offset] [-l length] [-f output_file] [-r|-x] [-n]\n", argv[0]);
            printf("  -d device       DAX device path (default: %s)\n", DEFAULT_DAX_DEVICE);
            printf("  -o offset       Read offset in bytes (default: 0)\n");
            printf("  -l length       Number of bytes to read (default: auto/all)\n");
            printf("  -f output_file  Output file (default: stdout)\n");
            printf("  -r              Raw mode (no header parsing)\n");
            printf("  -x              Hexdump mode\n");
            printf("  -n              No checksum verification\n");
            printf("  -h              Show this help\n");
            return 0;
        }
    }
    
    // Open output file if specified
    if (output_file) {
        output_fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (output_fd < 0) {
            fprintf(stderr, "Error creating output file %s: %s\n", 
                    output_file, strerror(errno));
            return EXIT_FAILURE;
        }
    }
    
    // Open DAX device
    fprintf(stderr, "Opening DAX device: %s\n", device);
    fd = open(device, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error opening device %s: %s\n", 
                device, strerror(errno));
        if (output_fd != STDOUT_FILENO) close(output_fd);
        return EXIT_FAILURE;
    }
    
    // Get device size
    if (fstat(fd, &sb) < 0) {
        fprintf(stderr, "Error getting device stats: %s\n", strerror(errno));
        close(fd);
        if (output_fd != STDOUT_FILENO) close(output_fd);
        return EXIT_FAILURE;
    }
    
    map_size = sb.st_size;
    fprintf(stderr, "Device size: %zu bytes\n", map_size);
    
    if (offset >= map_size) {
        fprintf(stderr, "Error: Offset %zu exceeds device size %zu\n", 
                offset, map_size);
        close(fd);
        if (output_fd != STDOUT_FILENO) close(output_fd);
        return EXIT_FAILURE;
    }
    
    // Memory map the device
    mapped_addr = mmap(NULL, map_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped_addr == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        close(fd);
        if (output_fd != STDOUT_FILENO) close(output_fd);
        return EXIT_FAILURE;
    }
    
    fprintf(stderr, "Successfully mapped at address: %p\n", mapped_addr);
    fprintf(stderr, "Reading from offset: %zu\n", offset);
    
    // Prepare read location
    uint8_t *read_ptr = (uint8_t *)mapped_addr + offset;
    size_t available_size = map_size - offset;
    
    if (raw_mode) {
        // Raw mode: read specified length or until end
        size_t read_length = length ? length : available_size;
        if (read_length > available_size) {
            read_length = available_size;
        }
        
        fprintf(stderr, "Reading %zu bytes in raw mode\n", read_length);
        
        if (raw_mode == 2) {
            // Hexdump mode
            hexdump(read_ptr, read_length, offset);
        } else {
            // Binary output mode
            if (write(output_fd, read_ptr, read_length) != (ssize_t)read_length) {
                fprintf(stderr, "Error writing output: %s\n", strerror(errno));
                munmap(mapped_addr, map_size);
                close(fd);
                if (output_fd != STDOUT_FILENO) close(output_fd);
                return EXIT_FAILURE;
            }
        }
    } else {
        // Header mode: parse DAX header
        if (available_size < sizeof(dax_header_t)) {
            fprintf(stderr, "Error: Not enough space for header at offset %zu\n", offset);
            munmap(mapped_addr, map_size);
            close(fd);
            if (output_fd != STDOUT_FILENO) close(output_fd);
            return EXIT_FAILURE;
        }
        
        dax_header_t *header = (dax_header_t *)read_ptr;
        
        // Verify magic
        if (strncmp(header->magic, "DAXDATA", 8) != 0) {
            fprintf(stderr, "Warning: Invalid magic signature at offset %zu\n", offset);
            fprintf(stderr, "Found: %.8s\n", header->magic);
            fprintf(stderr, "Use -r flag for raw mode\n");
            munmap(mapped_addr, map_size);
            close(fd);
            if (output_fd != STDOUT_FILENO) close(output_fd);
            return EXIT_FAILURE;
        }
        
        // Display header info
        fprintf(stderr, "=== DAX Data Header ===\n");
        fprintf(stderr, "Magic: %.8s\n", header->magic);
        fprintf(stderr, "Timestamp: %lu (%s)", header->timestamp, 
                ctime((time_t *)&header->timestamp));
        fprintf(stderr, "Data size: %lu bytes\n", header->data_size);
        fprintf(stderr, "Checksum: 0x%lx\n", header->checksum);
        
        // Verify data fits in available space
        if (header->data_size > available_size - sizeof(dax_header_t)) {
            fprintf(stderr, "Error: Data size in header exceeds available space\n");
            munmap(mapped_addr, map_size);
            close(fd);
            if (output_fd != STDOUT_FILENO) close(output_fd);
            return EXIT_FAILURE;
        }
        
        // Verify checksum if requested
        if (verify_checksum) {
            uint64_t calculated = calculate_checksum(header->data, header->data_size);
            if (calculated != header->checksum) {
                fprintf(stderr, "Warning: Checksum mismatch!\n");
                fprintf(stderr, "Expected: 0x%lx, Calculated: 0x%lx\n", 
                        header->checksum, calculated);
            } else {
                fprintf(stderr, "Checksum verified successfully\n");
            }
        }
        
        // Output data
        size_t output_length = length ? length : header->data_size;
        if (output_length > header->data_size) {
            output_length = header->data_size;
        }
        
        fprintf(stderr, "Writing %zu bytes of data\n", output_length);
        
        if (write(output_fd, header->data, output_length) != (ssize_t)output_length) {
            fprintf(stderr, "Error writing output: %s\n", strerror(errno));
            munmap(mapped_addr, map_size);
            close(fd);
            if (output_fd != STDOUT_FILENO) close(output_fd);
            return EXIT_FAILURE;
        }
    }
    
    // Cleanup
    if (output_fd != STDOUT_FILENO) close(output_fd);
    munmap(mapped_addr, map_size);
    close(fd);
    
    fprintf(stderr, "Read completed successfully\n");
    return EXIT_SUCCESS;
}

/*
 * Compilation:
 *   gcc -o dax_reader dax_reader.c
 *
 * Usage Examples:
 *   sudo ./dax_reader                          # Read header + data from offset 0
 *   sudo ./dax_reader -o 4096                  # Read from offset 4096
 *   sudo ./dax_reader -r -l 512                # Read raw 512 bytes
 *   sudo ./dax_reader -x -l 256                # Hexdump 256 bytes
 *   sudo ./dax_reader -f output.dat            # Save to file
 *   sudo ./dax_reader -d /dev/dax1.0 -n        # No checksum verification
 */