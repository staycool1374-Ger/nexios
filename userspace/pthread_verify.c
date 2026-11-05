/*
 * NexIOS RTOS — pthread phase-(ii) verification matrix (issue #53).
 * Live kernel primitives: exercises create/join/detach/exit round-trips
 * on SYS_THREAD_CREATE/WRAP/JOIN/EXIT, errno-cell isolation via the
 * trampoline TLS, both rendezvous orders, and fail-closed double/self
 * join. Marker lines ("PTHREAD_VERIFY: ...") are parsed by the
 * thread_pthread kernel class. Cond still fails closed (EVENT_WAIT
 * wiring is a follow-up).
 */

#include <stdio.h>
#include <errno.h>
#include <stddef.h>
#include <syscall.h>
#include "pthread.h"

static int g_counter;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void once_init(void) {
    g_counter += 100;
}

/* Round-trip worker: stores arg, returns a cookie. */
static void *round_trip_start(void *arg) {
    int *slot = (int *)arg;
    *slot = 0x5EED;
    return (void *)0x1234UL;
}

/* Slow worker: yields so the parent join blocks (JOIN-after-EXIT is
   covered by round_trip_start, which usually exits first). */
static volatile int g_slow_ready;
static void *slow_start(void *arg) {
    (void)arg;
    for (volatile unsigned long i = 0; i < 200000UL; ++i) {
        sys_yield();
    }
    g_slow_ready = 1;
    return (void *)0x5678UL;
}

/* Errno isolation worker: must not clobber the parent's errno cell. */
static void *errno_start(void *arg) {
    (void)arg;
    errno = 222;
    if (errno != 222) {
        return (void *)1UL;
    }
    return (void *)0UL;
}

int main(void) {
    int fails = 0;

    /* self / equal. */
    pthread_t self = pthread_self();
    if (self == 0 || !pthread_equal(self, pthread_self())) {
        printf("PTHREAD_VERIFY: self FAIL\n");
        ++fails;
    } else {
        printf("PTHREAD_VERIFY: self %lu\n", self);
    }

    /* create with bad args fails closed. */
    errno = 0;
    if (pthread_create(NULL, NULL, round_trip_start, NULL) != -1 ||
        errno != EINVAL) {
        printf("PTHREAD_VERIFY: create-null FAIL\n");
        ++fails;
    } else {
        printf("PTHREAD_VERIFY: create-null EINVAL\n");
    }
    {
        pthread_t tmp = 0;
        int attr = 0;
        errno = 0;
        if (pthread_create(&tmp, &attr, round_trip_start, NULL) != -1 ||
            errno != EINVAL) {
            printf("PTHREAD_VERIFY: create-attr FAIL\n");
            ++fails;
        } else {
            printf("PTHREAD_VERIFY: create-attr EINVAL\n");
        }
    }

    /* create / join round-trip with arg + status. */
    {
        pthread_t child = 0;
        int slot = 0;
        if (pthread_create(&child, NULL, round_trip_start, &slot) != 0) {
            printf("PTHREAD_VERIFY: create FAIL errno=%d\n", errno);
            ++fails;
        } else {
            printf("PTHREAD_VERIFY: create tid=%lu\n", child);
            void *retval = NULL;
            if (pthread_join(child, &retval) != 0) {
                printf("PTHREAD_VERIFY: join FAIL errno=%d\n", errno);
                ++fails;
            } else if (slot != 0x5EED || retval != (void *)0x1234UL) {
                printf("PTHREAD_VERIFY: roundtrip FAIL slot=%d retval=%p\n",
                       slot, retval);
                ++fails;
            } else {
                printf("PTHREAD_VERIFY: roundtrip ok\n");
            }
            /* Double join fails closed. */
            errno = 0;
            if (pthread_join(child, NULL) != -1 || errno != EINVAL) {
                printf("PTHREAD_VERIFY: double-join FAIL\n");
                ++fails;
            } else {
                printf("PTHREAD_VERIFY: double-join EINVAL\n");
            }
        }
    }

    /* self-join fails closed. */
    errno = 0;
    if (pthread_join(pthread_self(), NULL) != -1 || errno != EINVAL) {
        printf("PTHREAD_VERIFY: self-join FAIL\n");
        ++fails;
    } else {
        printf("PTHREAD_VERIFY: self-join EINVAL\n");
    }

    /* slow child: parent blocks in join until exit. */
    {
        pthread_t child = 0;
        g_slow_ready = 0;
        if (pthread_create(&child, NULL, slow_start, NULL) != 0) {
            printf("PTHREAD_VERIFY: slow-create FAIL errno=%d\n", errno);
            ++fails;
        } else {
            void *retval = NULL;
            if (pthread_join(child, &retval) != 0 ||
                retval != (void *)0x5678UL || g_slow_ready != 1) {
                printf("PTHREAD_VERIFY: slow-join FAIL\n");
                ++fails;
            } else {
                printf("PTHREAD_VERIFY: slow-join ok\n");
            }
        }
    }

    /* errno-cell isolation: child TP cell distinct from parent's. */
    {
        pthread_t child = 0;
        errno = 111;
        if (pthread_create(&child, NULL, errno_start, NULL) != 0) {
            printf("PTHREAD_VERIFY: errno-create FAIL\n");
            ++fails;
        } else {
            void *retval = (void *)1UL;
            if (pthread_join(child, &retval) != 0 || retval != (void *)0UL) {
                printf("PTHREAD_VERIFY: errno-join FAIL\n");
                ++fails;
            } else if (errno != 111) {
                printf("PTHREAD_VERIFY: errno-isolation FAIL errno=%d\n",
                       errno);
                ++fails;
            } else {
                printf("PTHREAD_VERIFY: errno-isolation ok\n");
            }
        }
    }

    /* detach: join-after-detach fails closed. */
    {
        pthread_t child = 0;
        if (pthread_create(&child, NULL, slow_start, NULL) != 0) {
            printf("PTHREAD_VERIFY: detach-create FAIL\n");
            ++fails;
        } else if (pthread_detach(child) != 0) {
            printf("PTHREAD_VERIFY: detach FAIL errno=%d\n", errno);
            ++fails;
        } else {
            errno = 0;
            if (pthread_join(child, NULL) != -1 || errno != EINVAL) {
                printf("PTHREAD_VERIFY: join-detached FAIL\n");
                ++fails;
            } else {
                printf("PTHREAD_VERIFY: detach ok\n");
            }
        }
    }

    /* mutex: exact exclusion counter, re-lock fails closed. */
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    int sum = 0;
    for (int i = 0; i < 500; ++i) {
        if (pthread_mutex_lock(&mutex) != 0) {
            ++fails;
            break;
        }
        ++sum;
        if (pthread_mutex_unlock(&mutex) != 0) {
            ++fails;
            break;
        }
    }
    if (sum != 500) {
        printf("PTHREAD_VERIFY: mutex FAIL sum=%d\n", sum);
        ++fails;
    } else {
        printf("PTHREAD_VERIFY: mutex 500\n");
    }
    if (pthread_mutex_lock(&mutex) != 0) {
        printf("PTHREAD_VERIFY: mutex relock setup FAIL\n");
        ++fails;
    } else if (pthread_mutex_lock(&mutex) != -1) {
        printf("PTHREAD_VERIFY: mutex relock FAIL\n");
        ++fails;
        pthread_mutex_unlock(&mutex);
    } else {
        printf("PTHREAD_VERIFY: relock EAGAIN\n");
        pthread_mutex_unlock(&mutex);
    }
    if (pthread_mutex_trylock(&mutex) != 0 ||
        pthread_mutex_unlock(&mutex) != 0 ||
        pthread_mutex_destroy(&mutex) != 0) {
        printf("PTHREAD_VERIFY: mutex lifecycle FAIL\n");
        ++fails;
    }

    /* once: exactly-once across repeated calls. */
    pthread_once(&g_once, once_init);
    pthread_once(&g_once, once_init);
    if (g_counter != 100) {
        printf("PTHREAD_VERIFY: once FAIL counter=%d\n", g_counter);
        ++fails;
    } else {
        printf("PTHREAD_VERIFY: once 100\n");
    }

    /* keys: round-trip + delete. */
    pthread_key_t key = 0;
    int marker = 42;
    if (pthread_key_create(&key, NULL) != 0 ||
        pthread_setspecific(key, &marker) != 0 ||
        pthread_getspecific(key) != &marker ||
        pthread_key_delete(key) != 0 || pthread_getspecific(key) != NULL) {
        printf("PTHREAD_VERIFY: keys FAIL\n");
        ++fails;
    } else {
        printf("PTHREAD_VERIFY: keys ok\n");
    }

    /* cond: rendezvous fails closed with ENOSYS (follow-up). */
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
    errno = 0;
    if (pthread_cond_wait(&cond, &wait_mutex) != -1 || errno != ENOSYS) {
        printf("PTHREAD_VERIFY: cond FAIL\n");
        ++fails;
    } else {
        printf("PTHREAD_VERIFY: cond ENOSYS\n");
    }

    if (fails == 0) {
        printf("PTHREAD_VERIFY: PASS\n");
    } else {
        printf("PTHREAD_VERIFY: FAIL %d\n", fails);
    }
    return fails;
}
