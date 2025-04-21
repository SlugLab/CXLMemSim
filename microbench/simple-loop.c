#include <stddef.h>

void simple_loop(int *a, size_t n) {
    size_t count = n / 4;
    asm volatile(
        ".intel_syntax noprefix\n\t"
        "mov rcx, %[cnt]         # rcx = count\n\t"
        "mov rdi, %[arr]         # rdi = &a[0]\n\t"
    "1:\n\t"
        "mov eax,  [rdi]         # load prev = a[i]\n\t"
        "add eax,  1             # prev += 1\n\t"
        "mov [rdi+4], eax        # a[i+1] = prev\n\t"
        "mov ebx,  eax           # ebx = prev\n\t"
        "mov [rdi+8], ebx        # a[i+2] = ebx\n\t"
        "mov ecx,  ebx           # ecx = ebx\n\t"
        "mov [rdi+12], ecx       # a[i+3] = ecx\n\t"
        "mov edx,  ecx           # edx = ecx\n\t"
        "mov [rdi+16], edx       # a[i+4] = edx\n\t"
        "add rdi,  16            # advance pointer by 4 elements\n\t"
        "sub rcx,  1             # count--\n\t"
        "jnz 1b                  # if (count) goto 1\n\t"
        ".att_syntax\n\t"
        :
        : [arr]"r"(a), [cnt]"r"(count)
        : "rax","rbx","rcx","rdx","rdi","memory"
    );
}

int main(int argc, char **argv) {
    int a[1000000];
    simple_loop(a, 1000000);
    return 0;
}