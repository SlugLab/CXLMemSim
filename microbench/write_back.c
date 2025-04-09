#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CACHE_SIZE_MB 32
#define ARRAY_SIZE (CACHE_SIZE_MB * 1024 * 1024 / sizeof(uint64_t))

int main() {
    uint64_t *arr = malloc(ARRAY_SIZE * sizeof(uint64_t));

    // Initialize the array to ensure memory pages are allocated
    memset(arr, 0, ARRAY_SIZE * sizeof(uint64_t));

    // First pass: Write to each element, marking cache lines dirty
    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        arr[i] = i;
    }

    // Second pass: Access another large memory region to evict previous lines
    uint64_t *arr2 = malloc(ARRAY_SIZE * sizeof(uint64_t));
    memset(arr2, 0, ARRAY_SIZE * sizeof(uint64_t));
    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        arr2[i] = i + 1;
    }

    // At this point, the first array's modified lines have likely been evicted,
    // resulting in LLC write-backs.

    printf("Write-back simulation completed.\n");

    free(arr);
    free(arr2);

    return 0;
}