#include <klee/klee.h>
#include <stdio.h>

#define N 20

int main(int argc, const char *argv[])
{
    int i, j;
    int v = 1;
    int a[N];

    for(i=0; i<N; ++i) {
        char name[8];
        sprintf(name, "a[%d]", i);
        a[i] = klee_int(name);
    }

    for(i=0; i<N; ++i) {
        if(a[i] >= 0) {
            printf("a[%d] >= 0\n", i);
        } else {
            printf("a[%d] < 0\n", i);
        }
    }

    return 0;
}

