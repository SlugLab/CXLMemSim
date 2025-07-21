#include <stddef.h>

void simple_loop(int *a, size_t n) {
    size_t count = n / 4;
    asm volatile(
        "mov rcx, %[cnt]         # rcx = count"
        "mov rdi, %[arr]         # rdi = &a[0]"
    "1:"
        "mov eax,  [rdi]         # load prev = a[i]"
        "add eax,  1             # prev += 1"
        "mov [rdi+4], eax        # a[i+1] = prev"
        "mov ebx,  eax           # ebx = prev"
        "mov [rdi+8], ebx        # a[i+2] = ebx"
        "mov ecx,  ebx           # ecx = ebx"
        "mov [rdi+12], ecx       # a[i+3] = ecx"
        "mov edx,  ecx           # edx = ecx"
        "mov [rdi+16], edx       # a[i+4] = edx"
        "add rdi,  16            # advance pointer by 4 elements"
        "sub rcx,  1             # count--"
        "jnz 1b                  # if (count) goto 1"
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