/*
 * Microbench testies for MLP and memory latency in CXLMS
 * Modified version with safer memory access
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *
 *  Copyright 2023 Regents of the University of California
 *  UC Santa Cruz Sluglab.
 */

#include <cstddef>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <cpuid.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <atomic>
#include <string.h>
#include <sys/mman.h>

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#define MOVE_SIZE 128
#define MAP_SIZE  (long)(1024)
#define CACHELINE_SIZE  64

#ifndef FENCE_COUNT
#define FENCE_COUNT 8
#endif

#define FENCE_BOUND (FENCE_COUNT * MOVE_SIZE)

// 确保内存访问不会出界的安全版本
#define BODY(start)                        \
"xor %%r8, %%r8 \n"                      \
"LOOP_START%=: \n"                       \
"lea (%[" #start "], %%r8), %%r9 \n"     \
"movdqa  (%%r9), %%xmm0 \n"              \
"movdqa %%xmm0, %%xmm1 \n"               \
"paddd %%xmm1, %%xmm0 \n"                \
"add $" STR(MOVE_SIZE) ", %%r8 \n"       \
"cmp $" STR(FENCE_BOUND) ",%%r8\n"       \
"jl LOOP_START%= \n"

int main(int argc, char **argv) {
  // 使用原子变量处理同步问题
  std::atomic<int> sync_var(0);

  // 分配更大的内存并确保对齐
  char *base =NULL;
  int ret = posix_memalign((void**)&base, CACHELINE_SIZE, MAP_SIZE);
  if (ret != 0 || base == nullptr) {
    fprintf(stderr, "Memory allocation failed: %d\n", ret);
    return -1;
  }

  if (base == MAP_FAILED) {
    fprintf(stderr, "Memory allocation failed: %d\n", errno);
    return -1;
  }

  // 确保内存对齐到缓存行
  uintptr_t addr_value = (uintptr_t)base;
  uintptr_t aligned_addr = (addr_value + CACHELINE_SIZE - 1) & ~(CACHELINE_SIZE - 1);
  char *aligned_base = (char*)aligned_addr;

  printf("Base address: %p, Aligned address: %p\n", base, aligned_base);

  // 初始化XMM0寄存器，避免使用未初始化的值
  char dummy_data[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
  asm volatile(
    "movdqu (%0), %%xmm0"
    :
    : "r" (dummy_data)
    : "xmm0"
  );

  char *addr = NULL;
  intptr_t *iaddr = (intptr_t*)aligned_base;
  intptr_t hash = 0;
  struct timespec tstart = {0,0}, tend = {0,0};

  // 初始化内存
  printf("Initializing memory...\n");
  size_t count = 0;
  while (iaddr < (intptr_t *)(aligned_base + MAP_SIZE)) {
    hash = hash ^ (intptr_t)iaddr;
    *iaddr = hash;
    iaddr++;
    count++;
  }
  printf("Initialized %zu intptr_t elements\n", count);

  // 使用普通内存操作替代缓存刷新
  printf("Flushing cache...\n");
  addr = aligned_base;
  count = 0;
  while (addr < (aligned_base + MAP_SIZE)) {
    // 使用读取+写入模式替代缓存刷新
    volatile char* vaddr = (volatile char*)addr;
    char temp = *vaddr;  // 读取到缓存
    *vaddr = temp;       // 写回以触发缓存状态变化

    // 使用C++原子操作确保内存排序
    sync_var.store(sync_var.load(std::memory_order_relaxed) + 1,
                  std::memory_order_release);

    addr += CACHELINE_SIZE;
    count++;
  }
  printf("Flushed %zu cache lines\n", count);
  // 确保之前的所有内存操作完成
  sync_var.load(std::memory_order_acquire);

  printf("Starting benchmark...\n");
  clock_gettime(CLOCK_MONOTONIC, &tstart);
  clock_gettime(CLOCK_MONOTONIC, &tstart);
for (int i=0;i<1e3;i++){
  addr = base;
  while (addr < (base + MAP_SIZE)) {
    asm volatile(
		 BODY(addr)
		 :
		 : [addr] "r" (addr)
		 : "r8", "r9", "xmm0");

      addr += (FENCE_COUNT * MOVE_SIZE);
  }
  clock_gettime(CLOCK_MONOTONIC, &tend);
  uint64_t nanos = (1000000000  * tend.tv_sec + tend.tv_nsec);
  nanos -= (1000000000 * tstart.tv_sec + tstart.tv_nsec);


  printf("%lu\n", nanos);
}
  // 解除内存映射
  munmap(base, MAP_SIZE + CACHELINE_SIZE);

  return 0;
}