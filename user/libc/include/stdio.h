#ifndef STDIO_H
#define STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
typedef struct {
    int fd, error, eof, pushed, memory;
    size_t position, length, capacity, read_position, read_length;
    unsigned char read_buffer[4096];
    char *data;
    char **result_data;
    size_t *result_size;
} FILE;
extern FILE *stdin, *stdout, *stderr;
#define BUFSIZ 4096
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IONBF 2
#define _IOLBF 1
#define _IOFBF 0
#define EOF (-1)
int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int puts(const char *);
int putchar(int);
int getchar(void);
void perror(const char *);
FILE *fdopen(int, const char *);
FILE *fopen(const char *, const char *);
int fclose(FILE *);
size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
int fseek(FILE *, long, int);
long ftell(FILE *);
int fflush(FILE *);
char *fgets(char *, int, FILE *);
ssize_t getline(char **, size_t *, FILE *);
FILE *open_memstream(char **, size_t *);
int fgetc(FILE *);
int getc(FILE *);
int ungetc(int, FILE *);
int fputc(int, FILE *);
int putc(int, FILE *);
int feof(FILE *);
int ferror(FILE *);
void clearerr(FILE *);
int fileno(FILE *);
int vfprintf(FILE *, const char *, va_list);
int vprintf(const char *, va_list);
int vsprintf(char *, const char *, va_list);
int sprintf(char *, const char *, ...);
int setvbuf(FILE *, char *, int, size_t);
int remove(const char *);
void rewind(FILE *);
int fputs(const char *, FILE *);
#endif
