#ifndef TIME_H
#define TIME_H
typedef long time_t;
typedef long clock_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCKS_PER_SEC 1000000L
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};
time_t time(time_t *);
char *ctime_r(const time_t *, char *);
struct tm *localtime(const time_t *);
struct tm *gmtime(const time_t *);
clock_t clock(void);
struct timespec {
    long tv_sec, tv_nsec;
};
int clock_gettime(int, struct timespec *);
int nanosleep(const struct timespec *, struct timespec *);
#endif
