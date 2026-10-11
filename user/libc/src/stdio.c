#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
static FILE streams[3] = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
FILE *stdin = &streams[0], *stdout = &streams[1], *stderr = &streams[2];
static void add(char *b, size_t n, size_t *pos, char c) {
    if (*pos + 1 < n) {
        b[*pos] = c;
    }
    (*pos)++;
}
int vsnprintf(char *b, size_t n, const char *format, va_list ap) {
    size_t pos = 0;
    while (*format) {
        if (*format != '%') {
            add(b, n, &pos, *format++);
            continue;
        }
        format++;
        char pad = *format == '0' ? '0' : ' ';
        unsigned width = 0, longs = 0;
        while (*format >= '0' && *format <= '9') {
            width = width * 10 + *format++ - '0';
        }
        while (*format == 'l' || *format == 'z') {
            longs++;
            format++;
        }
        char type = *format++;
        if (type == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            while (*s) {
                add(b, n, &pos, *s++);
            }
        } else if (type == 'c') {
            add(b, n, &pos, va_arg(ap, int));
        } else if (type == 'd' || type == 'i' || type == 'u' || type == 'x' || type == 'o' ||
                   type == 'p') {
            unsigned base = type == 'x' || type == 'p' ? 16 : type == 'o' ? 8 : 10;
            uint64_t v;
            bool negative = false;
            if (type == 'd' || type == 'i') {
                int64_t signed_value = longs ? va_arg(ap, long) : va_arg(ap, int);
                negative = signed_value < 0;
                v = negative ? -(uint64_t)signed_value : (uint64_t)signed_value;
            } else {
                v = type == 'p' ? (uint64_t)va_arg(ap, void *)
                    : longs     ? va_arg(ap, unsigned long)
                                : va_arg(ap, unsigned);
            }
            char digits[32];
            unsigned count = 0;
            do {
                digits[count++] = "0123456789abcdef"[v % base];
                v /= base;
            } while (v);
            if (negative) {
                add(b, n, &pos, '-');
            }
            if (type == 'p') {
                add(b, n, &pos, '0');
                add(b, n, &pos, 'x');
            }
            for (unsigned i = count + negative; i < width && i < 1024; i++) {
                add(b, n, &pos, pad);
            }
            while (count) {
                add(b, n, &pos, digits[--count]);
            }
        } else {
            add(b, n, &pos, type);
        }
    }
    if (n) {
        b[pos < n ? pos : n - 1] = 0;
    }
    return pos;
}
int snprintf(char *b, size_t n, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vsnprintf(b, n, f, ap);
    va_end(ap);
    return r;
}
static int print(FILE *stream, const char *fmt, va_list ap) {
    char small[256];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(small, sizeof(small), fmt, copy);
    va_end(copy);
    if (n < 0) {
        return -1;
    }
    char *buffer = small;
    if ((size_t)n >= sizeof(small)) {
        buffer = malloc((size_t)n + 1);
        if (!buffer) {
            return -1;
        }
        vsnprintf(buffer, (size_t)n + 1, fmt, ap);
    }
    size_t done = 0;
    while (done < (size_t)n) {
        ssize_t r = write(stream->fd, buffer + done, n - done);
        if (r <= 0) {
            if (buffer != small) {
                free(buffer);
            }
            return -1;
        }
        done += r;
    }
    if (buffer != small) {
        free(buffer);
    }
    return n;
}
int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = print(stdout, fmt, ap);
    va_end(ap);
    return r;
}
int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = print(f, fmt, ap);
    va_end(ap);
    return r;
}
int puts(const char *s) {
    if (fputs(s, stdout) < 0) {
        return EOF;
    }
    return putchar('\n');
}
int putchar(int c) {
    unsigned char b = c;
    return write(1, &b, 1) == 1 ? b : EOF;
}
int getchar(void) {
    unsigned char b;
    return read(0, &b, 1) == 1 ? b : EOF;
}
void perror(const char *s) {
    fprintf(stderr, "%s: %s\n", s, strerror(errno));
}
FILE *fopen(const char *p, const char *m) {
    int flags = *m == 'r'   ? O_RDONLY
                : *m == 'w' ? O_WRONLY | O_CREAT | O_TRUNC
                : *m == 'a' ? O_WRONLY | O_CREAT | O_APPEND
                            : -1;
    if (flags < 0) {
        errno = EINVAL;
        return NULL;
    }
    if (strchr(m, '+')) {
        flags = (flags & ~3) | O_RDWR;
    }
    int fd = open(p, flags, 0666);
    if (fd < 0) {
        return NULL;
    }
    FILE *f = calloc(1, sizeof(*f));
    if (!f) {
        close(fd);
        return NULL;
    }
    f->fd = fd;
    return f;
}
int fclose(FILE *f) {
    int r = close(f->fd);
    free(f);
    return r;
}
size_t fread(void *b, size_t s, size_t n, FILE *f) {
    if (!s || n > SIZE_MAX / s) {
        return 0;
    }
    ssize_t r = read(f->fd, b, s * n);
    if (r < 0) {
        f->error = 1;
        return 0;
    }
    if (!r) {
        f->eof = 1;
    }
    return r / s;
}
size_t fwrite(const void *b, size_t s, size_t n, FILE *f) {
    if (!s || n > SIZE_MAX / s) {
        return 0;
    }
    size_t total = s * n, done = 0;
    while (done < total) {
        ssize_t r = write(f->fd, (const char *)b + done, total - done);
        if (r <= 0) {
            f->error = 1;
            break;
        }
        done += r;
    }
    return done / s;
}
int fseek(FILE *f, long o, int w) {
    return lseek(f->fd, o, w) < 0 ? -1 : 0;
}
long ftell(FILE *f) {
    return lseek(f->fd, 0, SEEK_CUR);
}
int fflush(FILE *f) {
    (void)f;
    return 0;
}
char *fgets(char *b, int n, FILE *f) {
    if (n <= 1) {
        return NULL;
    }
    int i = 0;
    while (i < n - 1) {
        char c;
        ssize_t r = read(f->fd, &c, 1);
        if (r <= 0) {
            if (r < 0) {
                f->error = 1;
            } else {
                f->eof = 1;
            }
            break;
        }
        b[i++] = c;
        if (c == '\n') {
            break;
        }
    }
    b[i] = 0;
    return i ? b : NULL;
}
int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? (int)n : EOF;
}
