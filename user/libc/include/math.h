#ifndef MATH_H
#define MATH_H
#define M_PI 3.14159265358979323846
#define HUGE_VAL (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))
#define isfinite(x) __builtin_isfinite(x)
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define signbit(x) __builtin_signbit(x)
double sin(double);
double cos(double);
double tan(double);
double sqrt(double);
double floor(double);
double ceil(double);
double trunc(double);
double fabs(double);
double pow(double, double);
double log(double);
double exp(double);
double fmod(double, double);
double ldexp(double, int);
double frexp(double, int *);
double atan2(double, double);
float sinf(float);
float cosf(float);
float sqrtf(float);
float powf(float, float);
float fabsf(float);
float floorf(float);
float ceilf(float);
float ldexpf(float, int);
long double ldexpl(long double, int);
long double powl(long double, long double);
long double fabsl(long double);
#endif
