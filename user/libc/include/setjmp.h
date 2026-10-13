#ifndef SETJMP_H
#define SETJMP_H
#include <stdint.h>
typedef uint64_t jmp_buf[8];
int setjmp(jmp_buf) __attribute__((returns_twice));
_Noreturn void longjmp(jmp_buf, int);
#endif
