#ifndef _DG_SYS_TIME_H
#define _DG_SYS_TIME_H
#include <sys/types.h>
#include <time.h>
struct timeval {
    long tv_sec;
    long tv_usec;
};
struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};
int gettimeofday(struct timeval *tv, void *tz);
#endif
