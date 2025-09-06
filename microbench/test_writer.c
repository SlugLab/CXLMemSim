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
    
    // Write at offset 0 (within DAX range)
    strcpy(dax_addr, "hello world from writer");
    printf("Wrote: %s\n", (char*)dax_addr);
    
    munmap(dax_addr, page_size);
    close(fd);
    return 0;
}