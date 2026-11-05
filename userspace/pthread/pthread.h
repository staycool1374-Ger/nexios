/*
 * NexIOS RTOS — pthread userspace library (issue #53).
 * Phase (ii): birth/join/detach/exit wired to the kernel THREAD_CREATE /
 * THREAD_WRAP / THREAD_JOIN / THREAD_EXIT primitives (option Y: pthread_t
 * is the kernel tid; join wraps it to a ThreadCap in-kernel via WRAP).
 * Mutex slow path and condvars still fail closed (EVENT_WAIT wiring is a
 * follow-up); once/keys are functional.
 */

#pragma once

/// @brief Thread handle: the kernel thread id. Join authority is derived
/// in-kernel (WRAP mints the cap at join time); the raw id never enters
/// the join path as authority (audit S1(a)). Main thread id is 1.
typedef unsigned long pthread_t;
#define PTHREAD_MAIN_ID 1UL

/// @brief Non-recursive mutex. Phase (i): single-threaded test-and-set;
///        the blocking slow path (EVENT_WAIT) arrives in phase (ii).
///        Non-PI: must not guard RT sections (audit S2-2).
typedef struct {
    int lock_word;
    unsigned long owner;
} pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER {0, 0}

/// @brief Condition variable. Phase (i): rendezvous only — every wait and
///        signal fails closed with ENOSYS until phase (ii).
typedef struct {
    int waiters;
} pthread_cond_t;
#define PTHREAD_COND_INITIALIZER {0}

/// @brief One-time initializer flag.
typedef int pthread_once_t;
#define PTHREAD_ONCE_INIT 0

/// @brief Thread-specific-data key.
typedef unsigned int pthread_key_t;

int pthread_create(pthread_t *thread, const void *attr,
                   void *(*start)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);
void pthread_exit(void *retval);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);

int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_mutex_destroy(pthread_mutex_t *mutex);

int pthread_cond_init(pthread_cond_t *cond, const void *attr);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);
int pthread_cond_destroy(pthread_cond_t *cond);

int pthread_once(pthread_once_t *once, void (*init)(void));

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);
int pthread_key_delete(pthread_key_t key);
