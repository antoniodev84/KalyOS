#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>
static FILE streams[3] = {
    {.fd = 0, .pushed = -1}, {.fd = 1, .pushed = -1}, {.fd = 2, .pushed = -1}};
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
        bool left = false, alternate = false, plus = false;
        char pad = ' ';
        while (*format && strchr("-+# 0", *format)) {
            if (*format == '-') {
                left = true;
            }
            if (*format == '#') {
                alternate = true;
            }
            if (*format == '+') {
                plus = true;
            }
            if (*format == '0') {
                pad = '0';
            }
            format++;
        }
        int width = 0, precision = -1, longs = 0;
        if (*format == '*') {
            width = va_arg(ap, int);
            format++;
        } else {
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + *format++ - '0';
                if (width > 8192) {
                    return -1;
                }
            }
        }
        if (width < 0) {
            left = true;
            width = -width;
        }
        if (*format == '.') {
            format++;
            precision = 0;
            if (*format == '*') {
                precision = va_arg(ap, int);
                format++;
            } else {
                while (*format >= '0' && *format <= '9') {
                    precision = precision * 10 + *format++ - '0';
                    if (precision > 8192) {
                        return -1;
                    }
                }
            }
        }
        while (strchr("lzjt", *format) && *format) {
            longs++;
            format++;
        }
        if (*format == 'h') {
            format++;
            if (*format == 'h') {
                format++;
            }
        }
        bool long_double = *format == 'L';
        if (long_double) {
            format++;
        }
        char type = *format ? *format++ : 0;
        char text[8192], prefix[3];
        size_t length = 0, prefix_size = 0;
        const char *s = text;
        if (type == 's') {
            s = va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            while (s[length] && (precision < 0 || length < (size_t)precision)) {
                length++;
            }
        } else if (type == 'c' || type == '%') {
            text[0] = type == '%' ? '%' : va_arg(ap, int);
            length = 1;
        } else if (strchr("diuxXop", type) && type) {
            unsigned base = type == 'x' || type == 'X' || type == 'p' ? 16 : type == 'o' ? 8 : 10;
            uint64_t v;
            if (type == 'd' || type == 'i') {
                int64_t signed_value = longs ? va_arg(ap, long) : va_arg(ap, int);
                if (signed_value < 0) {
                    prefix[prefix_size++] = '-';
                    v = -(uint64_t)signed_value;
                } else {
                    v = signed_value;
                    if (plus) {
                        prefix[prefix_size++] = '+';
                    }
                }
            } else {
                v = type == 'p' ? (uint64_t)va_arg(ap, void *)
                    : longs     ? va_arg(ap, unsigned long)
                                : va_arg(ap, unsigned);
            }
            if ((alternate && v && base == 16) || type == 'p') {
                prefix[prefix_size++] = '0';
                prefix[prefix_size++] = type == 'X' ? 'X' : 'x';
            } else if (alternate && base == 8 && v) {
                prefix[prefix_size++] = '0';
            }
            char reverse[64];
            unsigned count = 0;
            if (v || precision != 0) {
                do {
                    reverse[count++] =
                        (type == 'X' ? "0123456789ABCDEF" : "0123456789abcdef")[v % base];
                    v /= base;
                } while (v);
            }
            int zeros = precision > (int)count ? precision - count : 0;
            int padding = width - prefix_size - count - zeros;
            if (!left && (pad != '0' || precision >= 0)) {
                while (padding-- > 0) {
                    add(b, n, &pos, ' ');
                }
            }
            for (size_t i = 0; i < prefix_size; i++) {
                add(b, n, &pos, prefix[i]);
            }
            if (!left && pad == '0' && precision < 0) {
                while (padding-- > 0) {
                    add(b, n, &pos, '0');
                }
            }
            while (zeros-- > 0) {
                add(b, n, &pos, '0');
            }
            while (count) {
                add(b, n, &pos, reverse[--count]);
            }
            if (left) {
                while (padding-- > 0) {
                    add(b, n, &pos, ' ');
                }
            }
            continue;
        } else if (strchr("fFeEgG", type) && type) {
            long double value = long_double ? va_arg(ap, long double) : va_arg(ap, double);
            if (value < 0) {
                text[length++] = '-';
                value = -value;
            }
            if (__builtin_isnan(value)) {
                memcpy(text + length, "nan", 3);
                length += 3;
            } else if (__builtin_isinf(value)) {
                memcpy(text + length, "inf", 3);
                length += 3;
            } else {
                int digits = precision < 0 ? 6 : precision > 32 ? 32 : precision;
                int exponent = 0;
                bool scientific = type == 'e' || type == 'E';
                if (type == 'g' || type == 'G') {
                    long double temp = value;
                    while (temp >= 10) {
                        temp /= 10;
                        exponent++;
                    }
                    while (temp && temp < 1) {
                        temp *= 10;
                        exponent--;
                    }
                    scientific = exponent < -4 || exponent >= (digits ? digits : 1);
                }
                if (scientific) {
                    exponent = 0;
                    while (value >= 10) {
                        value /= 10;
                        exponent++;
                    }
                    while (value && value < 1) {
                        value *= 10;
                        exponent--;
                    }
                }

                long double rounding = 0.5L;
                for (int i = 0; i < digits; i++) {
                    rounding /= 10;
                }
                value += rounding;
                long double power = 1;
                while (power * 10 <= value) {
                    power *= 10;
                }
                do {
                    int digit = value / power;
                    text[length++] = '0' + digit;
                    value -= digit * power;
                    power /= 10;
                } while (power >= 1);
                if (digits) {
                    text[length++] = '.';
                }
                for (int i = 0; i < digits; i++) {
                    value *= 10;
                    int digit = value;
                    text[length++] = '0' + digit;
                    value -= digit;
                }
                if (type == 'g' || type == 'G') {
                    while (length && text[length - 1] == '0') {
                        length--;
                    }
                    if (length && text[length - 1] == '.') {
                        length--;
                    }
                }
                if (scientific) {
                    text[length++] = 'e';
                    text[length++] = exponent < 0 ? '-' : '+';
                    unsigned ex = exponent < 0 ? -exponent : exponent;
                    char rev[8];
                    unsigned c = 0;
                    do {
                        rev[c++] = '0' + ex % 10;
                        ex /= 10;
                    } while (ex);
                    if (c < 2) {
                        rev[c++] = '0';
                    }
                    while (c) {
                        text[length++] = rev[--c];
                    }
                }
            }
        } else if (type == 'n') {
            if (longs) {
                *va_arg(ap, long *) = pos;
            } else {
                *va_arg(ap, int *) = pos;
            }
            continue;
        } else {
            errno = EINVAL;
            return -1;
        }
        int padding = width - (int)length;
        if (!left) {
            while (padding-- > 0) {
                add(b, n, &pos, ' ');
            }
        }
        for (size_t i = 0; i < length; i++) {
            add(b, n, &pos, s[i]);
        }
        if (left) {
            while (padding-- > 0) {
                add(b, n, &pos, ' ');
            }
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
        ssize_t r = fwrite(buffer + done, 1, n - done, stream);
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
    f->pushed = -1;
    return f;
}
int fclose(FILE *f) {
    int result = fflush(f);
    if (!f->memory && close(f->fd)) {
        result = -1;
    }
    if (f != stdin && f != stdout && f != stderr) {
        free(f);
    }
    return result;
}
int fgetc(FILE *f) {
    if (f->pushed >= 0) {
        int c = f->pushed;
        f->pushed = -1;
        return c;
    }
    if (f->memory) {
        if (f->position < f->length) {
            return (unsigned char)f->data[f->position++];
        }
        f->eof = 1;
        return EOF;
    }
    if (f->read_position == f->read_length) {
        ssize_t n = read(f->fd, f->read_buffer, sizeof(f->read_buffer));
        if (n <= 0) {
            if (n < 0) {
                f->error = 1;
            } else {
                f->eof = 1;
            }
            return EOF;
        }
        f->read_position = 0;
        f->read_length = n;
    }
    return f->read_buffer[f->read_position++];
}
int getc(FILE *f) {
    return fgetc(f);
}
int ungetc(int c, FILE *f) {
    if (c == EOF || f->pushed >= 0) {
        return EOF;
    }
    f->pushed = (unsigned char)c;
    f->eof = 0;
    return f->pushed;
}
size_t fread(void *b, size_t s, size_t n, FILE *f) {
    if (!s || n > SIZE_MAX / s) {
        return 0;
    }
    size_t total = s * n, done = 0;
    while (done < total) {
        int c = fgetc(f);
        if (c == EOF) {
            break;
        }
        ((unsigned char *)b)[done++] = c;
    }
    return done / s;
}
size_t fwrite(const void *b, size_t s, size_t n, FILE *f) {
    if (!s || n > SIZE_MAX / s) {
        return 0;
    }
    size_t total = s * n, done = 0;
    if (f->memory) {
        if (total > SIZE_MAX - f->position - 1) {
            errno = EOVERFLOW;
            f->error = 1;
            return 0;
        }
        size_t needed = f->position + total + 1;
        if (needed > f->capacity) {
            size_t capacity = needed < f->capacity * 2 ? f->capacity * 2 : needed;
            char *p = realloc(f->data, capacity);
            if (!p) {
                f->error = 1;
                return 0;
            }
            f->data = p;
            f->capacity = capacity;
        }
        memcpy(f->data + f->position, b, total);
        f->position += total;
        if (f->position > f->length) {
            f->length = f->position;
        }
        f->data[f->length] = 0;
        fflush(f);
        return n;
    }
    if (f->read_position < f->read_length || f->pushed >= 0) {
        if (fseek(f, 0, SEEK_CUR)) {
            return 0;
        }
    }
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
int fseek(FILE *f, long offset, int origin) {
    if (f->memory) {
        long base = origin == SEEK_SET   ? 0
                    : origin == SEEK_CUR ? (long)f->position
                    : origin == SEEK_END ? (long)f->length
                                         : -1;
        if (base < 0 || offset < -base || offset > 0x7fffffff - base) {
            errno = EINVAL;
            return -1;
        }
        size_t position = base + offset;
        if (position > f->length) {
            size_t old = f->position;
            f->position = f->length;
            char zero[256] = {0};
            while (f->length < position) {
                size_t n = position - f->length;
                if (n > sizeof(zero)) {
                    n = sizeof(zero);
                }
                if (fwrite(zero, 1, n, f) != n) {
                    f->position = old;
                    return -1;
                }
            }
        }
        f->position = position;
    } else {
        if (origin == SEEK_CUR) {
            offset -= (long)(f->read_length - f->read_position) + (f->pushed >= 0);
        }
        if (lseek(f->fd, offset, origin) < 0) {
            return -1;
        }
        f->read_position = f->read_length = 0;
    }
    f->pushed = -1;
    f->eof = 0;
    return 0;
}
long ftell(FILE *f) {
    return f->memory ? (long)f->position
                     : lseek(f->fd, 0, SEEK_CUR) - (long)(f->read_length - f->read_position) -
                           (f->pushed >= 0);
}
int fflush(FILE *f) {
    if (!f) {
        return fflush(stdout) || fflush(stderr) ? EOF : 0;
    }
    if (f->memory) {
        *f->result_data = f->data;
        *f->result_size = f->length;
    }
    return f->error ? EOF : 0;
}
FILE *open_memstream(char **data, size_t *size) {
    if (!data || !size) {
        errno = EINVAL;
        return NULL;
    }
    FILE *f = calloc(1, sizeof(*f));
    if (!f) {
        return NULL;
    }
    f->data = malloc(128);
    if (!f->data) {
        free(f);
        return NULL;
    }
    f->fd = -1;
    f->memory = 1;
    f->pushed = -1;
    f->capacity = 128;
    f->result_data = data;
    f->result_size = size;
    f->data[0] = 0;
    fflush(f);
    return f;
}
char *fgets(char *b, int n, FILE *f) {
    if (n <= 1) {
        return NULL;
    }
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) {
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
int fputc(int c, FILE *f) {
    unsigned char b = c;
    return fwrite(&b, 1, 1, f) == 1 ? b : EOF;
}
int putc(int c, FILE *f) {
    return fputc(c, f);
}
int feof(FILE *f) {
    return f->eof;
}
int ferror(FILE *f) {
    return f->error;
}
void clearerr(FILE *f) {
    f->error = f->eof = 0;
}
int fileno(FILE *f) {
    return f->fd;
}
void rewind(FILE *f) {
    fseek(f, 0, SEEK_SET);
    clearerr(f);
}
int setvbuf(FILE *f, char *b, int mode, size_t size) {
    (void)b;
    (void)size;
    if (mode != _IONBF && mode != _IOFBF && mode != _IOLBF) {
        errno = EINVAL;
        return -1;
    }
    return fflush(f);
}
int remove(const char *path) {
    struct stat st;
    return stat(path, &st) ? -1 : S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
}
int vfprintf(FILE *f, const char *fmt, va_list ap) {
    return print(f, fmt, ap);
}
int vprintf(const char *fmt, va_list ap) {
    return print(stdout, fmt, ap);
}
int vsprintf(char *b, const char *fmt, va_list ap) {
    return vsnprintf(b, (size_t)-1, fmt, ap);
}
int sprintf(char *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(b, fmt, ap);
    va_end(ap);
    return n;
}
ssize_t getline(char **line, size_t *capacity, FILE *stream) {
    if (!line || !capacity || !stream) {
        errno = EINVAL;
        return -1;
    }
    if (!*line || !*capacity) {
        char *p = malloc(128);
        if (!p) {
            return -1;
        }
        *line = p;
        *capacity = 128;
    }
    size_t length = 0;
    for (;;) {
        int c = fgetc(stream);
        if (c == EOF) {
            break;
        }
        if (length + 1 >= *capacity) {
            if (*capacity > SIZE_MAX / 2) {
                errno = EOVERFLOW;
                return -1;
            }
            char *p = realloc(*line, *capacity * 2);
            if (!p) {
                return -1;
            }
            *line = p;
            *capacity *= 2;
        }
        (*line)[length++] = c;
        if (c == '\n') {
            break;
        }
    }
    (*line)[length] = 0;
    return length ? (ssize_t)length : -1;
}
FILE *fdopen(int fd, const char *mode) {
    if (fd < 0 || !mode || !strchr("rwa", *mode)) {
        errno = EINVAL;
        return NULL;
    }
    if (fcntl(fd, F_GETFL) < 0) {
        return NULL;
    }
    FILE *f = calloc(1, sizeof(*f));
    if (f) {
        f->fd = fd;
        f->pushed = -1;
    }
    return f;
}
