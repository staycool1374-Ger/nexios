#include <syscall.h>
#include <time.h>
#include <errno.h>

extern int errno;

/// @brief §2 error predicate (picolib_stubs.c map_err shape): map a raw
/// syscall return to errno/-1.
static long map_err(long ret) {
    if (ret < 0 && (unsigned long)ret > -4096UL) {
        errno = (int)(-ret);
        return -1;
    }
    return ret;
}

time_t time(time_t* tloc) {
    struct timespec ts;
    if (map_err(sys_clock_gettime(CLOCK_REALTIME, &ts)) < 0) {
        // Fallback one release: legacy gettod path (then remove).
        struct timeval tv;
        if (sys_gettod(&tv) < 0) return (time_t)-1;
        if (tloc) *tloc = tv.tv_sec;
        return tv.tv_sec;
    }
    if (tloc) *tloc = ts.tv_sec;
    return ts.tv_sec;
}

int gettimeofday(struct timeval* tv, void* tz) {
    struct timespec ts;
    (void)tz;
    if (!tv) return -1;
    if (map_err(sys_clock_gettime(CLOCK_REALTIME, &ts)) < 0) {
        // Fallback one release: legacy gettod path (then remove).
        return (int)sys_gettod(tv);
    }
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = (long)(ts.tv_nsec / 1000);
    return 0;
}

int clock_gettime(int clockid, struct timespec* tp) {
    if (!tp) {
        errno = EINVAL;
        return -1;
    }
    return (int)map_err(sys_clock_gettime((unsigned long)clockid, tp));
}

unsigned int sleep(unsigned int seconds) {
    struct timespec req;
    struct timespec rem;
    req.tv_sec = seconds;
    req.tv_nsec = 0;
    // EINTR loop: resume with the remainder (kernel fills rem).
    while (map_err(sys_nanosleep(&req, &rem)) < 0) {
        if (errno != EINTR) return seconds;
        req = rem;
        seconds = (unsigned int)rem.tv_sec;
    }
    return 0;
}

unsigned int alarm(unsigned int seconds) {
    return (unsigned int)sys_alarm(seconds);
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!req) {
        errno = EINVAL;
        return -1;
    }
    return (int)map_err(sys_nanosleep(req, rem));
}