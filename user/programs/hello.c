#include <stdio.h>
#include <unistd.h>
int main(void) {
    printf("Hello from ELF64 dynamic, pid %d\n", getpid());
    return 0;
}
