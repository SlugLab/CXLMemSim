#include "uarch.h"

int main() {
    int i;
    long long aggregated = 0, aggregated2 = 0;
    long seed = 0xdeadbeef1245678;
    uint64_t a = 0xfc0;
    int access_size = 64;
    int stride_size = 64;
    int delay = 64;
    int count = 32;
    uint64_t *cindex;
    uint64_t csize;
    int ret;

    for (i = 0; i < 100000; i++) {
        char *buf = static_cast<char *>(malloc(4096 * 1024));
        buf = buf + 64 - (((long)buf) % 64);
        // Separate RaW job
        RAW_BEFORE_WRITE
        stride_storeclwb(buf, access_size, stride_size, delay, count);
        asm volatile("mfence \n" :::);
        RAW_BEFORE_READ
        stride_nt(buf, access_size, stride_size, delay, count);
        asm volatile("mfence \n" :::);
        RAW_FINAL("raw-separate")

        aggregated += diff;
        aggregated2 += c_ntload_end - c_store_start;
    }

        printf("Separate RaW job %lld %lld\n", aggregated / 100000 / count, aggregated2 / 100000 / count);
    return 0;
}
