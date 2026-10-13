#ifndef SYS_TIME_H
#define SYS_TIME_H
#include <time.h>
struct timeval {
    long tv_sec, tv_usec;
};
struct timezone {
    int tz_minuteswest, tz_dsttime;
};
int gettimeofday(struct timeval *, void *);
#endif
