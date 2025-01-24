#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// 定义一个远大于典型LLC大小的数组大小(如32MB)
// 假设每个缓存行是64字节，LLC是8MB
#define ARRAY_SIZE (32 * 1024 * 1024)  // 32MB
#define STRIDE 64  // 以缓存行大小作为步长
#define ITERATIONS 1000000000

int main() {
    // 分配大数组
    int *large_array = (int *)malloc(ARRAY_SIZE * sizeof(int));
    if (large_array == NULL) {
        printf("内存分配失败\n");
        return 1;
    }

    // 初始化随机数生成器
    srand(time(NULL));

    // 初始化数组
    for (int i = 0; i < ARRAY_SIZE; i++) {
        large_array[i] = i;
    }

    // 记录开始时间
    clock_t start = clock();
    
    // 随机访问数组以造成缓存未命中
    long long sum = 0;  // 防止编译器优化掉访问操作
    for (long long i = 0; i < ITERATIONS; i++) {
        // 随机选择一个索引，确保跨越缓存行
        int index = (rand() % (ARRAY_SIZE / STRIDE)) * STRIDE;
        sum += large_array[index];
    }

    // 记录结束时间
    clock_t end = clock();
    double time_spent = (double)(end - start) / CLOCKS_PER_SEC;

    printf("执行时间: %.2f 秒\n", time_spent);
    printf("Sum: %lld (防止优化)\n", sum);

    // 清理
    free(large_array);
    return 0;
}