#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main()
{
    size_t page_size = 2 * 1024 * 1024;  // 2MB DAX alignment
    
    int fd = open("/dev/dax0.0", O_RDWR);
    if (fd == -1) {
        perror("open() failed");
        return 1;
    }
    
    void *dax_addr = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (dax_addr == MAP_FAILED) {
        perror("mmap() failed");
        close(fd);
        return 1;
    }
    
    // Read from offset 0
    char buffer[100];
    strncpy(buffer, dax_addr, 99);
    buffer[99] = '\0';
    
    printf("Read: %s\n", buffer);
    
    // Show hex dump of first 32 bytes
    printf("Hex: ");
    unsigned char *p = (unsigned char*)dax_addr;
    for (int i = 0; i < 32; i++) {
        printf("%02x ", p[i]);
    }
    printf("\n");
    
    munmap(dax_addr, page_size);
    close(fd);
    return 0;
}