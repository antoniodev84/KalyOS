#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdbool.h>
void *malloc(size_t n) {
    if (n > SIZE_MAX - 4112) {
        errno = ENOMEM;
        return NULL;
    }
    size_t size = (n + 16 + 4095) & ~4095UL;
    size_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    p[0] = size;
    p[1] = n;
    return p + 2;
}
void free(void *p) {
    if (p) {
        size_t *h = (size_t *)p - 2;
        munmap(h, h[0]);
    }
}
void *calloc(size_t n, size_t s) {
    if (s && n > SIZE_MAX / s) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = malloc(n * s);
    if (p) {
        memset(p, 0, n * s);
    }
    return p;
}
void *realloc(void *p, size_t n) {
    if (!p) {
        return malloc(n);
    }
    if (!n) {
        free(p);
        return NULL;
    }
    size_t old = ((size_t *)p)[-1];
    void *q = malloc(n);
    if (q) {
        memcpy(q, p, old < n ? old : n);
        free(p);
    }
    return q;
}
unsigned long strtoul(const char *s, char **end, int base) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    bool negative = *s == '-';
    if (*s == '-' || *s == '+') {
        s++;
    }
    if ((!base || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (!base) {
        base = *s == '0' ? 8 : 10;
    }
    unsigned long value = 0;
    while (*s) {
        unsigned d = *s >= '0' && *s <= '9'   ? *s - '0'
                     : *s >= 'a' && *s <= 'z' ? *s - 'a' + 10
                     : *s >= 'A' && *s <= 'Z' ? *s - 'A' + 10
                                              : 36;
        if (d >= (unsigned)base) {
            break;
        }
        value = value * base + d;
        s++;
    }
    if (end) {
        *end = (char *)s;
    }
    return negative ? -value : value;
}
long strtol(const char *s, char **end, int base) {
    return strtoul(s, end, base);
}
int atoi(const char *s) {
    return strtol(s, NULL, 10);
}
char *getenv(const char *name) {
    size_t n = strlen(name);
    for (char **e = environ; e && *e; e++) {
        if (!strncmp(*e, name, n) && (*e)[n] == '=') {
            return *e + n + 1;
        }
    }
    return NULL;
}
static void (*exit_handlers[64])(void);
static unsigned exit_count;
int atexit(void (*callback)(void)) {
    if (exit_count == 64) {
        errno = ENOMEM;
        return -1;
    }
    exit_handlers[exit_count++] = callback;
    return 0;
}
_Noreturn void exit(int n) {
    while (exit_count) {
        exit_handlers[--exit_count]();
    }
    _exit(n);
}
