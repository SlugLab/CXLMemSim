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

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#define MOVE_SIZE 128
#define MAP_SIZE  (long)(1024 * 1024 * 2) // 加倍内存大小以确保安全
#define CACHELINE_SIZE  64

#ifndef FENCE_COUNT
#define FENCE_COUNT 8
#endif

#define FENCE_BOUND (FENCE_COUNT * MOVE_SIZE)

// 确保内存访问不会出界的安全版本
#define BODY(start) \
  "xor %%r8, %%r8 \n" \
  "LOOP_START%=: \n" \
  "lea (%[" #start "], %%r8), %%r9 \n" \
  "movntdq %%xmm0, (%%r9) \n" \
  "add $" STR(MOVE_SIZE) ", %%r8 \n" \
  "cmp $" STR(FENCE_BOUND) ",%%r8\n" \
  "jl LOOP_START%= \n"  \
  "mfence \n" 

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

  // should flush everything from the cache. But, how big is the cache?
  addr = base;
  while (addr < (base + MAP_SIZE)) {
    asm volatile(
		 "mov %[buf], %%rsi\n"
		 "clflush (%%rsi)\n"
          "mfence\n"
		 :
		 : [buf] "r" (addr)
		 : "rsi");
    addr += CACHELINE_SIZE;
  }


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
  return 0;
}