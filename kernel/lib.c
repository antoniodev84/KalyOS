#include "kaly.h"
void *memset(void *p, int c, size_t n) {
    uint8_t *d = p;
    while (n--) {
        *d++ = c;
    }
    return p;
}
void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--) {
        *a++ = *b++;
    }
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    if (d < s) {
        return memcpy(d, s, n);
    }
    uint8_t *a = (uint8_t *)d + n;
    const uint8_t *b = (const uint8_t *)s + n;
    while (n--) {
        *--a = *--b;
    }
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) {
        if (*x != *y) {
            return *x - *y;
        }
        x++;
        y++;
    }
    return 0;
}
size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n--) {
        if (*a != *b || !*a) {
            return (unsigned char)*a - (unsigned char)*b;
        }
        a++;
        b++;
    }
    return 0;
}
char *strcpy(char *d, const char *s) {
    char *p = d;
    while ((*d++ = *s++))
        ;
    return p;
}
char *strchr(const char *s, int c) {
    do {
        if (*s == c) {
            return (char *)s;
        }
    } while (*s++);
    return NULL;
}
uint64_t strtou(const char *s, unsigned base) {
    uint64_t v = 0;
    while (*s) {
        unsigned d = *s >= '0' && *s <= '9'   ? (unsigned)(*s - '0')
                     : *s >= 'a' && *s <= 'f' ? (unsigned)(*s - 'a' + 10)
                     : *s >= 'A' && *s <= 'F' ? (unsigned)(*s - 'A' + 10)
                                              : base;
        if (d >= base) {
            break;
        }
        v = v * base + d;
        s++;
    }
    return v;
}
void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt++ != '%') {
            console_putc(fmt[-1]);
            continue;
        }
        char c = *fmt++;
        if (c == 's') {
            char *s = va_arg(ap, char *);
            console_write(s, strlen(s));
        } else if (c == 'c') {
            console_putc(va_arg(ap, int));
        } else if (c == 'x' || c == 'u') {
            uint64_t v = va_arg(ap, uint64_t);
            char b[32];
            unsigned n = 0, base = c == 'x' ? 16 : 10;
            do {
                unsigned d = v % base;
                b[n++] = "0123456789abcdef"[d];
                v /= base;
            } while (v);
            while (n) {
                console_putc(b[--n]);
            }
        } else {
            console_putc(c);
        }
    }
    va_end(ap);
}
_Noreturn void panic(const char *s) {
    __asm__ volatile("cli");
    kprintf("panic: %s\n", s);
    for (;;) {
        __asm__ volatile("hlt");
    }
}
