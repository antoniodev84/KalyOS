#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
double fabs(double x) {
    return __builtin_fabs(x);
}
long double fabsl(long double x) {
    return __builtin_fabsl(x);
}
float fabsf(float x) {
    return fabs(x);
}
double sqrt(double x) {
    double r;
    __asm__("sqrtsd %1,%0" : "=x"(r) : "x"(x));
    return r;
}
float sqrtf(float x) {
    return sqrt(x);
}
double fmod(double x, double y) {
    if (!isfinite(x) || isnan(y) || !y) {
        return NAN;
    }
    if (isinf(y)) {
        return x;
    }
    long double r;
    __asm__ volatile(
        "fldt %2; fldt %1; 1: fprem; fnstsw %%ax; test $1024,%%ax; jnz 1b; fstpt %0; fstp %%st(0)"
        : "=m"(r)
        : "m"((long double){x}), "m"((long double){y})
        : "ax", "st", "st(1)");
    return r;
}
double sin(double x) {
    double r;
    x = fmod(x, 6.28318530717958647692);
    __asm__("fldl %1; fsin; fstpl %0" : "=m"(r) : "m"(x) : "st");
    return r;
}
double cos(double x) {
    double r;
    x = fmod(x, 6.28318530717958647692);
    __asm__("fldl %1; fcos; fstpl %0" : "=m"(r) : "m"(x) : "st");
    return r;
}
double tan(double x) {
    return sin(x) / cos(x);
}
float sinf(float x) {
    return sin(x);
}
float cosf(float x) {
    return cos(x);
}
double trunc(double x) {
    if (!isfinite(x) || fabs(x) >= 0x1p63) {
        return x;
    }
    return (double)(long long)x;
}
double floor(double x) {
    double t = trunc(x);
    return t > x ? t - 1 : t;
}
double ceil(double x) {
    double t = trunc(x);
    return t < x ? t + 1 : t;
}
float floorf(float x) {
    return floor(x);
}
float ceilf(float x) {
    return ceil(x);
}
long double ldexpl(long double x, int n) {
    long double r, exponent = n;
    __asm__("fldt %2; fldt %1; fscale; fstpt %0; fstp %%st(0)"
            : "=m"(r)
            : "m"(x), "m"(exponent)
            : "st", "st(1)");
    return r;
}
double ldexp(double x, int n) {
    return ldexpl(x, n);
}
float ldexpf(float x, int n) {
    return ldexpl(x, n);
}
double frexp(double x, int *n) {
    *n = 0;
    if (!x || !isfinite(x)) {
        return x;
    }
    while (fabs(x) >= 1) {
        x *= 0.5;
        (*n)++;
    }
    while (fabs(x) < 0.5) {
        x *= 2;
        (*n)--;
    }
    return x;
}
double log(double x) {
    double r;
    __asm__("fldln2; fldl %1; fyl2x; fstpl %0" : "=m"(r) : "m"(x) : "st", "st(1)");
    return r;
}
static long double exponential(long double x) {
    if (isnan(x)) {
        return x;
    }
    if (x > 11356) {
        return HUGE_VALL;
    }
    if (x < -11400) {
        return 0;
    }
    long double scaled = x * 1.44269504088896340736L;
    int n = (int)scaled;
    long double fraction = scaled - n, value;
    __asm__("fldt %1; f2xm1; fld1; faddp; fstpt %0" : "=m"(value) : "m"(fraction) : "st", "st(1)");
    return ldexpl(value, n);
}
double exp(double x) {
    return exponential(x);
}
long double powl(long double x, long double y) {
    if (y == 0 || x == 1) {
        return 1;
    }
    if (isnan(x) || isnan(y)) {
        return NAN;
    }
    if (!x) {
        return y > 0 ? 0 : HUGE_VALL;
    }
    int negative = x < 0;
    if (negative && (y >= 0x1p63L || y < -0x1p63L || y != (long long)y)) {
        return NAN;
    }
    long double value;
    long double base = negative ? -x : x;
    __asm__("fldln2; fldt %1; fyl2x; fstpt %0" : "=m"(value) : "m"(base) : "st", "st(1)");
    value = exponential(value * y);
    return negative && ((long long)y & 1) ? -value : value;
}
double pow(double x, double y) {
    return powl(x, y);
}
float powf(float x, float y) {
    return pow(x, y);
}
double atan2(double y, double x) {
    double r;
    __asm__("fldl %1; fldl %2; fpatan; fstpl %0" : "=m"(r) : "m"(y), "m"(x) : "st", "st(1)");
    return r;
}
long double strtold(const char *s, char **end) {
    const char *start = s;
    while (isspace((unsigned char)*s)) {
        s++;
    }
    int sign = 1;
    if (*s == '-' || *s == '+') {
        if (*s++ == '-') {
            sign = -1;
        }
    }
    if (!strncasecmp(s, "inf", 3)) {
        s += 3;
        if (!strncasecmp(s, "inity", 5)) {
            s += 5;
        }
        if (end) {
            *end = (char *)s;
        }
        return sign * HUGE_VALL;
    }
    if (!strncasecmp(s, "nan", 3)) {
        s += 3;
        if (end) {
            *end = (char *)s;
        }
        return NAN;
    }
    int base = 10, digits = 0, fraction = 0, point = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    long double value = 0;
    for (;;) {
        int digit = isdigit(*s)                                ? *s - '0'
                    : tolower(*s) >= 'a' && tolower(*s) <= 'f' ? tolower(*s) - 'a' + 10
                                                               : 99;
        if (digit < base) {
            value = value * base + digit;
            digits++;
            fraction += point;
            s++;
        } else if (*s == '.' && !point) {
            point = 1;
            s++;
        } else {
            break;
        }
    }
    if (!digits) {
        if (end) {
            *end = (char *)start;
        }
        return 0;
    }
    int exponent = 0;
    if ((base == 10 && (*s == 'e' || *s == 'E')) || (base == 16 && (*s == 'p' || *s == 'P'))) {
        const char *mark = s++;
        int esign = 1;
        if (*s == '+' || *s == '-') {
            if (*s++ == '-') {
                esign = -1;
            }
        }
        if (!isdigit(*s)) {
            s = mark;
        } else {
            while (isdigit(*s)) {
                if (exponent < 20000) {
                    exponent = exponent * 10 + *s - '0';
                }
                s++;
            }
            exponent *= esign;
        }
    }
    int power = exponent - fraction * (base == 16 ? 4 : 1);
    if (base == 16) {
        value = ldexpl(value, power);
    } else {
        long double scale = 10, factor = 1;
        unsigned n = power < 0 ? -power : power;
        while (n) {
            if (n & 1) {
                factor *= scale;
            }
            scale *= scale;
            n >>= 1;
        }
        value = power < 0 ? value / factor : value * factor;
    }
    if (!isfinite(value) || (!value && digits && power < -4900)) {
        errno = ERANGE;
    }
    if (end) {
        *end = (char *)s;
    }
    return sign * value;
}
double strtod(const char *s, char **end) {
    return strtold(s, end);
}
float strtof(const char *s, char **end) {
    return strtold(s, end);
}
