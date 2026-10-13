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
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
double strtod(const char *, char **);
float strtof(const char *, char **);
long double strtold(const char *, char **);
void *bsearch(const void *, const void *, size_t, size_t, int (*)(const void *, const void *));
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
int mkstemp(char *);
_Noreturn void abort(void);
int abs(int);
long labs(long);
char *getenv(const char *);
int atexit(void (*)(void));
_Noreturn void exit(int);
#endif
