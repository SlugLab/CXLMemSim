// simple-loop-no-dep.c
// gcc -O2 -masm=intel simple-loop-no-dep.c -o simple-loop-no-dep

#include <stddef.h>

void simple_loop_no_dep(int *a, size_t n) {
    size_t count = n / 4;
    asm volatile(
        "mov rcx, %[cnt]         # rcx = count"
        "mov rdi, %[arr]         # rdi = &a[0]"
    "1:"
        "mov eax,  0             # start from 0 (no dependency)"
        "add eax,  1             # eax = 1"
        "mov [rdi+4], eax        # a[i+1] = 1"
        "mov ebx,  eax           # ebx = 1"
        "mov [rdi+8], ebx        # a[i+2] = 1"
        "mov ecx,  ebx           # ecx = 1"
        "mov [rdi+12], ecx       # a[i+3] = 1"
        "mov edx,  ecx           # edx = 1"
        "mov [rdi+16], edx       # a[i+4] = 1"
        "add rdi,  16            # advance pointer by 4 elements"
        "sub rcx,  1             # count--"
        "jnz 1b                  # if (count) goto 1"
        ".att_syntax"
        :
        : [arr]"r"(a), [cnt]"r"(count)
        : "rax","rbx","rcx","rdx","rdi","memory"
    );
}

int main(int argc, char **argv) {
    int a[1000000];
    simple_loop_no_dep(a, 1000000);
    return 0;
}
