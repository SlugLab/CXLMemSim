#include <stdio.h>
#include <stdlib.h>
int main() {
    int *p;
#pragma omp parallel for
    for (int i = 0; i < 10 * 1024 * 1024; i++) {
        int inc = 1024 * sizeof(char);
        p = (int *)calloc(1, inc);
        if (!p)
            printf("error");
    }
}