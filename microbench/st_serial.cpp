/*
 * Microbench testies for MLP and memory latency in CXLMS
 *
 *  By: Andrew Quinn
 *      Yiwei Yang
 *
 *  Copyright 2023 Regents of the Univeristy of California
 *  UC Santa Cruz Sluglab.
 */
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
// we need to jump in MOVE_SIZE increments otherwise segfault!
// 修改BODY宏，去掉clflush和mfence，但保持指令顺序
#define BODY(start)						\
  "xor %%r8, %%r8 \n"						\
  "pxor %%xmm1, %%xmm1 \n"					\
  "LOOP_START%=: \n"						\
  "lea (%[" #start "], %%r8), %%r9 \n"				\
  "movdqa  %%xmm1, (%%r9) \n"					\
  "add $" STR(MOVE_SIZE) ", %%r8 \n"				\
  "cmp $" STR(FENCE_BOUND) ",%%r8\n"				\
  "mov $0, %%eax \n"                    \
  "cpuid \n"                            \
  "jl LOOP_START%= \n"

int main(int argc, char **argv) {
  // in principle, you would want to clear out cache lines (and the
  // pipeline) before doing any of the inline assembly stuff.  But,
  // that's hard.  And, its probably noise when you execute over
  // enough things.
  // allocate some meomery
  char *base =(char *) mmap(nullptr,
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
  // Necessary so that we don't include allocation costs in our benchmark
  while (iaddr < (intptr_t *)(base + MAP_SIZE)) {
    hash = hash ^ (intptr_t) iaddr;
    *iaddr = hash;
    iaddr++;
  }
  
  // 替代缓存刷新的代码段
  addr = base;
  // 使用大块内存访问替代缓存刷新
  size_t cache_clear_size = 32 * 1024 * 1024; // 大于典型的L3缓存
  char *cache_clear = (char *)malloc(cache_clear_size);
  if (cache_clear) {
    volatile char temp = 0;
    for (size_t i = 0; i < cache_clear_size; i += CACHELINE_SIZE) {
      cache_clear[i] = (char)i;
      temp += cache_clear[i]; // 确保访问不被优化掉
    }
    free(cache_clear);
  }
  
  // 使用cpuid替代内存屏障
  unsigned int eax, ebx, ecx, edx;
  __cpuid(0, eax, ebx, ecx, edx);
  
  clock_gettime(CLOCK_MONOTONIC, &tstart);
  for (int i=0;i<1e3;i++){
  addr = base;
  while (addr < (base + MAP_SIZE)) {
    //fprintf (stderr, "addr %p bound %p\n", addr, base + MAP_SIZE);
    asm volatile(
		 BODY(addr)
		 :
		 : [addr] "r" (addr)
		 : "rax", "rbx", "rcx", "rdx", "r8", "r9", "xmm0", "xmm1", "memory");
      addr += (FENCE_COUNT * MOVE_SIZE);
  }
  clock_gettime(CLOCK_MONOTONIC, &tend);
  uint64_t nanos = (1000000000  * tend.tv_sec + tend.tv_nsec);
  nanos -= (1000000000 * tstart.tv_sec + tstart.tv_nsec);
  printf("%lu\n", nanos);
  }
  
  munmap(base, MAP_SIZE);
  return 0;
}