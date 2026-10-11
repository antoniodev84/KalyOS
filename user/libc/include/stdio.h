#ifndef STDIO_H
#define STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
typedef struct {
    int fd, error, eof;
} FILE;
extern FILE *stdin, *stdout, *stderr;
#define EOF (-1)
int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int puts(const char *);
int putchar(int);
int getchar(void);
void perror(const char *);
FILE *fopen(const char *, const char *);
int fclose(FILE *);
size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
int fseek(FILE *, long, int);
long ftell(FILE *);
int fflush(FILE *);
char *fgets(char *, int, FILE *);
int fputs(const char *, FILE *);
#endif
