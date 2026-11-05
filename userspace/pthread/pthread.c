/*
 * NexIOS RTOS — pthread userspace library (issue #53).
 * Phase (ii): birth/join/detach/exit wired to SYS_THREAD_CREATE, the
 * option-Y join path (WRAP-then-JOIN inside pthread_join), SYS_THREAD_EXIT,
 * and SYS_TLS_SET. Mutex/once/keys as in phase (i); the mutex slow path
 * and condvars still fail closed (EVENT_WAIT wiring is a follow-up).
 */

#include "pthread.h"
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <syscall.h>
#include <unistd.h>

/// @brief Default thread stack: the kernel maximum (64 KiB); pthread
/// passes the byte count and the kernel owns the VA (audit S2-1).
/// Attributes are not supported yet (attr must be NULL).
#define PTHREAD_DEFAULT_STACK_BYTES (64UL * 1024UL)

/// @brief Birth context: allocated by pthread_create in shared memory
/// (same address space — visible to the child), planted in the child's
/// first C argument register by the kernel (option Y). The trampoline
/// consumes it, publishes TLS, and runs start(arg).
struct thread_birth {
    void *(*start)(void *);
    void *arg;
    /* TP block (struct nexios_tcb layout: errno at +0, thread id
       appended). Published via TLS_SET before any libc call. */
    struct nexios_tcb tcb;
};

/// @brief Thread entry trampoline (issue #53): the kernel dispatches the
/// child here with (birth context, own tid). TLS first (errno must work
/// before start runs), then start(arg), then exit with the return value.
/// On the pthread_exit path the context block leaks (bounded: one small
/// block per exited-without-return thread, reclaimed at process exit).
static void pthread_trampoline(struct thread_birth *ctx, unsigned long tid) {
    ctx->tcb.errno_value = 0;
    ctx->tcb.thread_id = tid;
    sys_tls_set((unsigned long)&ctx->tcb);
    void *ret = ctx->start(ctx->arg);
    free(ctx);
    sys_thread_exit((unsigned long)ret);
    for (;;) {
    }
}

int pthread_create(pthread_t *thread, const void *attr,
                   void *(*start)(void *), void *arg) {
    if (thread == NULL || start == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (attr != NULL) {
        /* No attribute support in phase (ii). */
        errno = EINVAL;
        return -1;
    }
    struct thread_birth *ctx =
        (struct thread_birth *)malloc(sizeof(struct thread_birth));
    if (ctx == NULL) {
        errno = EAGAIN;
        return -1;
    }
    ctx->start = start;
    ctx->arg = arg;
    ctx->tcb.errno_value = 0;
    ctx->tcb.thread_id = 0;
    long tid = sys_thread_create(0, (unsigned long)pthread_trampoline,
                                 PTHREAD_DEFAULT_STACK_BYTES, 0,
                                 (unsigned long)ctx);
    if (tid < 0) {
        free(ctx);
        errno = EAGAIN;
        return -1;
    }
    *thread = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    unsigned long status = 0;
    /* Option Y: the raw id never enters the join path as authority — the
       kernel WRAPs it to a ThreadCap internally... */
    long r = sys_thread_wrap((unsigned long)thread, 0);
    if (r < 0) {
        errno = EINVAL;
        return -1;
    }
    unsigned long cap = (unsigned long)r;
    r = sys_thread_join(cap, 0, (unsigned long)&status);
    if (r < 0) {
        errno = EINVAL;
        return -1;
    }
    if (retval != NULL) {
        *retval = (void *)status;
    }
    return 0;
}

int pthread_detach(pthread_t thread) {
    long r = sys_thread_wrap((unsigned long)thread, 0);
    if (r < 0) {
        errno = EINVAL;
        return -1;
    }
    if (sys_thread_join((unsigned long)r, 1, 0) < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void pthread_exit(void *retval) {
    sys_thread_exit((unsigned long)retval);
    for (;;) {
    }
}

pthread_t pthread_self(void) {
    /* TP-published id (trampoline sets it; crt0 leaves it zero for the
       main thread, which maps to PTHREAD_MAIN_ID). */
    unsigned long tid = __pthread_self_tid();
    return tid != 0 ? (pthread_t)tid : PTHREAD_MAIN_ID;
}

/// @brief Thread-specific-data slots (phase i: process-wide, one thread).
#define PTHREAD_MAX_KEYS 32

static void *g_key_values[PTHREAD_MAX_KEYS];
static int g_key_used[PTHREAD_MAX_KEYS];
static unsigned int g_key_next;

int pthread_equal(pthread_t a, pthread_t b) {
    return (a == b) ? 1 : 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr) {
    (void)attr;
    if (mutex == NULL) {
        errno = EINVAL;
        return -1;
    }
    mutex->lock_word = 0;
    mutex->owner = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (mutex == NULL) {
        errno = EINVAL;
        return -1;
    }
    // Phase (i): single thread — one atomic claim attempt, no blocking
    // path (no bare spins per project discipline; contention is
    // impossible with one thread and fails closed instead of wedging).
    // __atomic_* (never __sync_*): the legacy __sync builtins lower to
    // libatomic/libgcc calls (e.g. __aarch64_cas4_sync) that do not exist
    // freestanding; the __atomic family inlines on all three arches.
    int expected = 0;
    if (!__atomic_compare_exchange_n(&mutex->lock_word, &expected, 1, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        errno = EAGAIN;
        return -1;
    }
    mutex->owner = PTHREAD_MAIN_ID;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    return pthread_mutex_lock(mutex);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (mutex == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (mutex->lock_word == 0 || mutex->owner != PTHREAD_MAIN_ID) {
        errno = EPERM;
        return -1;
    }
    mutex->owner = 0;
    __atomic_store_n(&mutex->lock_word, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (mutex == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (mutex->lock_word != 0) {
        // EBUSY has no in-tree code; EAGAIN is the fail-closed equivalent.
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, const void *attr) {
    (void)attr;
    if (cond == NULL) {
        errno = EINVAL;
        return -1;
    }
    cond->waiters = 0;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    (void)cond;
    (void)mutex;
    // No kernel rendezvous yet (phase ii: EVENT_WAIT-based wait).
    errno = ENOSYS;
    return -1;
}

int pthread_cond_signal(pthread_cond_t *cond) {
    (void)cond;
    errno = ENOSYS;
    return -1;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    (void)cond;
    errno = ENOSYS;
    return -1;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    if (cond == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (cond->waiters != 0) {
        // EBUSY has no in-tree code; EAGAIN is the fail-closed equivalent.
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

int pthread_once(pthread_once_t *once, void (*init)(void)) {
    if (once == NULL || init == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (*once == 0) {
        *once = 1;
        init();
    }
    return 0;
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    (void)destructor; // Phase (i): no thread death, destructors unwired.
    if (key == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned int i = 0; i < PTHREAD_MAX_KEYS; ++i) {
        unsigned int slot = (g_key_next + i) % PTHREAD_MAX_KEYS;
        if (!g_key_used[slot]) {
            g_key_used[slot] = 1;
            g_key_values[slot] = NULL;
            g_key_next = (slot + 1) % PTHREAD_MAX_KEYS;
            *key = slot;
            return 0;
        }
    }
    errno = EAGAIN;
    return -1;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    if (key >= PTHREAD_MAX_KEYS || !g_key_used[key]) {
        errno = EINVAL;
        return -1;
    }
    g_key_values[key] = (void *)value;
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    if (key >= PTHREAD_MAX_KEYS || !g_key_used[key]) {
        return NULL;
    }
    return g_key_values[key];
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= PTHREAD_MAX_KEYS || !g_key_used[key]) {
        errno = EINVAL;
        return -1;
    }
    g_key_used[key] = 0;
    g_key_values[key] = NULL;
    return 0;
}
