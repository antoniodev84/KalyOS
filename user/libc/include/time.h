#ifndef TIME_H
#define TIME_H
struct timespec {
    long tv_sec, tv_nsec;
};
int nanosleep(const struct timespec *, struct timespec *);
#endif
