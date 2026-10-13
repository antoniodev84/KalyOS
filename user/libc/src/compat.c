#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <math.h>
int isspace(int c) {
    return c == ' ' || (c >= 9 && c <= 13);
}
int isdigit(int c) {
    return c >= '0' && c <= '9';
}
int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int islower(int c) {
    return c >= 'a' && c <= 'z';
}
int isupper(int c) {
    return c >= 'A' && c <= 'Z';
}
int isalpha(int c) {
    return islower(c) || isupper(c);
}
int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}
int isprint(int c) {
    return c >= 32 && c <= 126;
}
int iscntrl(int c) {
    return (c >= 0 && c < 32) || c == 127;
}
int ispunct(int c) {
    return isprint(c) && !isalnum(c) && c != ' ';
}
int tolower(int c) {
    return isupper(c) ? c + 32 : c;
}
int toupper(int c) {
    return islower(c) ? c - 32 : c;
}
int abs(int x) {
    return x < 0 ? -x : x;
}
long labs(long x) {
    return x < 0 ? -x : x;
}
_Noreturn void abort(void) {
    _exit(134);
}
void __assert_fail(const char *e, const char *file, int line, const char *function) {
    fprintf(stderr, "%s:%d: %s: assertion %s failed\n", file, line, function, e);
    abort();
}
int strncasecmp(const char *a, const char *b, size_t n) {
    while (n--) {
        int x = tolower((unsigned char)*a++), y = tolower((unsigned char)*b++);
        if (x != y || !x) {
            return x - y;
        }
    }
    return 0;
}
int strcasecmp(const char *a, const char *b) {
    return strncasecmp(a, b, (size_t)-1);
}
void bzero(void *p, size_t n) {
    memset(p, 0, n);
}
void *memchr(const void *p, int c, size_t n) {
    const unsigned char *s = p;
    while (n--) {
        if (*s == (unsigned char)c) {
            return (void *)s;
        }
        s++;
    }
    return NULL;
}
char *strndup(const char *s, size_t n) {
    size_t len = 0;
    while (len < n && s[len]) {
        len++;
    }
    char *p = malloc(len + 1);
    if (p) {
        memcpy(p, s, len);
        p[len] = 0;
    }
    return p;
}
char *strstr(const char *s, const char *needle) {
    size_t n = strlen(needle);
    for (; *s; s++) {
        if (!strncmp(s, needle, n)) {
            return (char *)s;
        }
    }
    return n ? NULL : (char *)s;
}
size_t strspn(const char *s, const char *set) {
    size_t n = 0;
    while (s[n] && strchr(set, s[n])) {
        n++;
    }
    return n;
}
size_t strcspn(const char *s, const char *set) {
    size_t n = 0;
    while (s[n] && !strchr(set, s[n])) {
        n++;
    }
    return n;
}
char *strpbrk(const char *s, const char *set) {
    s += strcspn(s, set);
    return *s ? (char *)s : NULL;
}
char *strtok_r(char *s, const char *set, char **save) {
    if (!s) {
        s = *save;
    }
    if (!s) {
        return NULL;
    }
    s += strspn(s, set);
    if (!*s) {
        *save = NULL;
        return NULL;
    }
    char *end = s + strcspn(s, set);
    if (*end) {
        *end++ = 0;
    }
    *save = end;
    return s;
}
char *strtok(char *s, const char *set) {
    static char *save;
    return strtok_r(s, set, &save);
}
char *strncat(char *a, const char *b, size_t n) {
    char *end = a + strlen(a);
    while (n-- && *b) {
        *end++ = *b++;
    }
    *end = 0;
    return a;
}
char *basename(char *s) {
    static char dot[] = ".";
    if (!s || !*s) {
        return dot;
    }
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') {
        s[--n] = 0;
    }
    char *p = strrchr(s, '/');
    return p && p[1] ? p + 1 : s;
}
char *dirname(char *s) {
    static char dot[] = ".";
    if (!s || !*s) {
        return dot;
    }
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') {
        s[--n] = 0;
    }
    char *p = strrchr(s, '/');
    if (!p) {
        return dot;
    }
    while (p > s && p[-1] == '/') {
        p--;
    }
    if (p == s) {
        s[1] = 0;
    } else {
        *p = 0;
    }
    return s;
}
long long strtoll(const char *s, char **end, int base) {
    return strtol(s, end, base);
}
unsigned long long strtoull(const char *s, char **end, int base) {
    return strtoul(s, end, base);
}
static void swap(unsigned char *a, unsigned char *b, size_t size) {
    while (size--) {
        unsigned char c = *a;
        *a++ = *b;
        *b++ = c;
    }
}
static void sift(unsigned char *b, size_t root, size_t count, size_t size,
                 int (*cmp)(const void *, const void *)) {
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && cmp(b + child * size, b + (child + 1) * size) < 0) {
            child++;
        }
        if (cmp(b + root * size, b + child * size) >= 0) {
            break;
        }
        swap(b + root * size, b + child * size, size);
        root = child;
    }
}
void qsort(void *base, size_t count, size_t size, int (*cmp)(const void *, const void *)) {
    if (!size || count < 2 || count > (size_t)-1 / size) {
        return;
    }
    unsigned char *b = base;
    for (size_t i = count / 2; i; i--) {
        sift(b, i - 1, count, size, cmp);
    }
    for (size_t n = count; n > 1; n--) {
        swap(b, b + (n - 1) * size, size);
        sift(b, 0, n - 1, size, cmp);
    }
}
int mkstemp(char *path) {
    static unsigned sequence;
    size_t n = strlen(path);
    if (n < 6 || strcmp(path + n - 6, "XXXXXX")) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned attempt = 0; attempt < 4096; attempt++) {
        unsigned long value = (unsigned long)getpid() * 4096 + ++sequence;
        for (unsigned i = 0; i < 6; i++) {
            path[n - 1 - i] = "0123456789abcdefghijklmnopqrstuvwxyz"[value % 36];
            value /= 36;
        }
        int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) {
            return fd;
        }
    }
    errno = EEXIST;
    return -1;
}
int access(const char *path, int mode) {
    struct stat st;
    if (stat(path, &st)) {
        return -1;
    }
    if (mode & ~7) {
        errno = EINVAL;
        return -1;
    }
    unsigned bits = getuid() == st.st_uid ? (st.st_mode >> 6) & 7 : st.st_mode & 7;
    if ((!getuid() && (!(mode & X_OK) || (st.st_mode & 0111))) || (bits & mode) == (unsigned)mode) {
        return 0;
    }
    errno = EACCES;
    return -1;
}
int isatty(int fd) {
    struct stat st;
    if (fstat(fd, &st)) {
        return 0;
    }
    if ((st.st_mode & S_IFMT) == 0020000 && st.st_rdev == 1) {
        return 1;
    }
    errno = ENOTTY;
    return 0;
}
time_t time(time_t *t) {
    return syscall(201, (long)t, 0, 0, 0, 0, 0);
}
int clock_gettime(int id, struct timespec *t) {
    return syscall(228, id, (long)t, 0, 0, 0, 0);
}
int gettimeofday(struct timeval *v, void *tz) {
    struct timespec t;
    if (clock_gettime(CLOCK_REALTIME, &t)) {
        return -1;
    }
    if (v) {
        v->tv_sec = t.tv_sec;
        v->tv_usec = t.tv_nsec / 1000;
    }
    if (tz) {
        memset(tz, 0, sizeof(struct timezone));
    }
    return 0;
}
clock_t clock(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : t.tv_sec * CLOCKS_PER_SEC + t.tv_nsec / 1000;
}
struct tm *gmtime(const time_t *value) {
    static struct tm out;
    long days = *value / 86400, seconds = *value % 86400;
    if (seconds < 0) {
        seconds += 86400;
        days--;
    }
    out.tm_hour = seconds / 3600;
    out.tm_min = seconds / 60 % 60;
    out.tm_sec = seconds % 60;
    out.tm_wday = (days + 4) % 7;
    if (out.tm_wday < 0) {
        out.tm_wday += 7;
    }
    long year = 1970;
    while (days < 0) {
        year--;
        days += !(year % 4) && (year % 100 || !(year % 400)) ? 366 : 365;
    }
    for (;;) {
        int n = !(year % 4) && (year % 100 || !(year % 400)) ? 366 : 365;
        if (days < n) {
            break;
        }
        days -= n;
        year++;
    }
    out.tm_year = year - 1900;
    out.tm_yday = days;
    out.tm_mon = 0;
    out.tm_isdst = 0;
    static const int months[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    while (out.tm_mon < 11) {
        int n =
            months[out.tm_mon] + (out.tm_mon == 1 && !(year % 4) && (year % 100 || !(year % 400)));
        if (days < n) {
            break;
        }
        days -= n;
        out.tm_mon++;
    }
    out.tm_mday = days + 1;
    return &out;
}
struct tm *localtime(const time_t *t) {
    return gmtime(t);
}
char *ctime_r(const time_t *time_value, char *buffer) {
    struct tm *t = gmtime(time_value);
    const char *week[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char *month[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(buffer, 26, "%s %s %2d %02d:%02d:%02d %04d\n", week[t->tm_wday], month[t->tm_mon],
             t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, t->tm_year + 1900);
    return buffer;
}
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *)) {
    const unsigned char *p = base;
    if (!size || count > (size_t)-1 / size) {
        return NULL;
    }
    while (count) {
        size_t middle = count / 2;
        const void *value = p + middle * size;
        int c = compare(key, value);
        if (!c) {
            return (void *)value;
        }
        if (c < 0) {
            count = middle;
        } else {
            p += (middle + 1) * size;
            count -= middle + 1;
        }
    }
    return NULL;
}
