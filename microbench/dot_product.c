#include <stdio.h>

int dot_product(int *a, int *b, int n) {
    int sum = 0;

    for (int i = 0; i < n; i++) {
        /*
         * We cast 'i' to a long so the compiler will put it in a 64-bit
         * register (RCX). Then we use 'S' to place 'a' into RSI,
         * and 'D' to place 'b' into RDI. This way the memory operands
         * (%%rsi,%%rcx,4) and (%%rdi,%%rcx,4) become valid.
         */
        long idx = i;
        asm volatile (
            "movl %[sum], %%eax           \n\t"   // sum -> EAX
            "movl (%%rsi, %%rcx, 4), %%edx \n\t"   // EDX = a[i]
            "movl (%%rdi, %%rcx, 4), %%ecx \n\t"   // ECX = b[i]
            "imull %%ecx, %%edx           \n\t"   // EDX = EDX * ECX
            "addl %%edx, %%eax            \n\t"   // EAX += EDX
            "movl %%eax, %[sum]           \n\t"   // sum = EAX
            : [sum]"+m"(sum)                      // output (sum) is read & written
            : "S"(a), "D"(b), "c"(idx)            // input: a->RSI, b->RDI, idx->RCX
            : "eax", "edx"                // clobbered registers
        );
    }

    return sum;
}

int main(void) {
    int a[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int b[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    int n = sizeof(a) / sizeof(a[0]);

    int result = dot_product(a, b, n);
    printf("Dot product: %d\n", result);  // Expected: 130

    return 0;
}
