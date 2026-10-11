#ifndef MMAN_H
#define MMAN_H
#include <stddef.h>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_FIXED 16
#define MAP_ANONYMOUS 32
#define MAP_FAILED ((void *)-1)
void *mmap(void *, size_t, int, int, int, long);
int mprotect(void *, size_t, int);
int munmap(void *, size_t);
int brk(void *);
void *sbrk(long);
#endif
