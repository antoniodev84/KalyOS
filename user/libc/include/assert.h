#ifndef ASSERT_H
#define ASSERT_H
#include <stdlib.h>
void __assert_fail(const char *, const char *, int, const char *);
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__, __func__))
#endif
#endif
