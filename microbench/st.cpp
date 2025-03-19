#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <cpuid.h>
#include <pthread.h>
#include <stdlib.h>
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

// 修改的BODY宏，去除所有fence指令
#define BODY(start)                     \
  "xor %%r8, %%r8 \n"                   \
  "pxor %%xmm1, %%xmm1 \n"              \
  "LOOP_START%=: \n"                    \
  "lea (%[" #start "], %%r8), %%r9 \n"  \
  "movdqa  %%xmm1, (%%r9) \n"           \
  "add $" STR(MOVE_SIZE) ", %%r8 \n"    \
  "cmp $" STR(FENCE_BOUND) ",%%r8\n"    \
  "jl LOOP_START%= \n"                  \
  "mov $0, %%eax \n"                    \
  "cpuid \n"                            // 使用cpuid作为序列点替代内存屏障

int main(int argc, char **argv) {
  char *base = (char *) mmap(nullptr,
                            MAP_SIZE,
                            PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_PRIVATE,
                            -1,
                            0);
  if (base == MAP_FAILED) {
    fprintf(stderr, "oops, you suck %d\n", errno);
    return -1;
  }

  char *addr = NULL;
  intptr_t *iaddr = (intptr_t*) base;
  intptr_t hash = 0;
  struct timespec tstart = {0,0}, tend = {0,0};

  // 填充内存以确保页面分配
  while (iaddr < (intptr_t *)(base + MAP_SIZE)) {
    hash = hash ^ (intptr_t) iaddr;
    *iaddr = hash;
    iaddr++;
  }

  // 清除缓存的替代方案：访问比缓存大的内存区域
  size_t cache_clear_size = 32 * 1024 * 1024; // 大于典型的L3缓存
  char *cache_clear = (char *)malloc(cache_clear_size);
  if (cache_clear) {
    volatile char temp = 0;
    // 使用循环方式访问内存，驱逐之前的缓存内容
    for (size_t i = 0; i < cache_clear_size; i += CACHELINE_SIZE) {
      cache_clear[i] = (char)i;
      temp += cache_clear[i]; // 确保访问不被优化掉
    }
    free(cache_clear);
  }

  // 使用cpuid指令作为序列点
  unsigned int eax, ebx, ecx, edx;
  __cpuid(0, eax, ebx, ecx, edx);

  clock_gettime(CLOCK_MONOTONIC, &tstart);
  for (int i=0;i<1e3;i++){
  addr = base;

  while (addr < (base + MAP_SIZE)) {
    asm volatile(
      BODY(addr)
      :
      : [addr] "r" (addr)
      : "rax", "rbx", "rcx", "rdx", "r8", "r9", "xmm0", "xmm1", "memory");
    addr += (FENCE_COUNT * MOVE_SIZE);
  }

  clock_gettime(CLOCK_MONOTONIC, &tend);
  uint64_t nanos = (1000000000 * tend.tv_sec + tend.tv_nsec);
  nanos -= (1000000000 * tstart.tv_sec + tstart.tv_nsec);
  printf("%lu\n", nanos);
  }

  munmap(base, MAP_SIZE);
  return 0;
}