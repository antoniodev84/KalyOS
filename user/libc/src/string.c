#include <string.h>
#include <stdlib.h>
void *memset(void *p, int c, size_t n) {
    unsigned char *s = p;
    while (n--) {
        *s++ = c;
    }
    return p;
}
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--) {
        *a++ = *b++;
    }
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    if (d < s) {
        return memcpy(d, s, n);
    }
    unsigned char *a = (unsigned char *)d + n;
    const unsigned char *b = (const unsigned char *)s + n;
    while (n--) {
        *--a = *--b;
    }
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
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
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}
char *strncpy(char *d, const char *s, size_t n) {
    char *r = d;
    while (n && *s) {
        *d++ = *s++;
        n--;
    }
    while (n--) {
        *d++ = 0;
    }
    return r;
}
char *strcat(char *d, const char *s) {
    strcpy(d + strlen(d), s);
    return d;
}
char *strchr(const char *s, int c) {
    do {
        if (*s == c) {
            return (char *)s;
        }
    } while (*s++);
    return NULL;
}
char *strrchr(const char *s, int c) {
    char *r = NULL;
    do {
        if (*s == c) {
            r = (char *)s;
        }
    } while (*s++);
    return r;
}
char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    return r ? memcpy(r, s, n) : NULL;
}
char *strerror(int e) {
    switch (e) {
    case 1:
        return "Operation not permitted";
    case 2:
        return "No such file";
    case 5:
        return "I/O error";
    case 8:
        return "Invalid executable";
    case 9:
        return "Bad descriptor";
    case 10:
        return "No child";
    case 11:
        return "Try again";
    case 12:
        return "Out of memory";
    case 13:
        return "Permission denied";
    case 14:
        return "Bad address";
    case 16:
        return "Busy";
    case 17:
        return "Already exists";
    case 18:
        return "Cross-device link";
    case 20:
        return "Not a directory";
    case 21:
        return "Is a directory";
    case 22:
        return "Invalid argument";
    case 24:
        return "Too many descriptors";
    case 27:
        return "File too large";
    case 28:
        return "No space";
    case 29:
        return "Illegal seek";
    case 32:
        return "Broken pipe";
    case 34:
        return "Out of range";
    case 38:
        return "Unsupported syscall";
    case 39:
        return "Directory not empty";
    case 40:
        return "Too many symbolic links";
    default:
        return "Error";
    }
}
