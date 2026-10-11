#ifndef STDLIB_H
#define STDLIB_H
#include <stddef.h>
void *malloc(size_t);
void free(void *);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
int atoi(const char *);
char *getenv(const char *);
int atexit(void (*)(void));
_Noreturn void exit(int);
#endif
