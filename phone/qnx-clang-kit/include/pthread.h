/* Minimal QNX 6.5 pthreads for upstream's MOST tunnel (src/most).
 * [ref] from upstream's qcc-built worker: mutex and cond are 8 bytes (g_mu at
 * +4, g_cv at +12); condattr lives in a 40-byte frame, so 32 is generous;
 * libc only ever gets pointers, so oversized storage is harmless. */
#ifndef QNX65_PTHREAD_H
#define QNX65_PTHREAD_H
#include "_qnx65.h"
typedef int pthread_t;                                          /* [doc] thread id */
typedef struct { int __count; unsigned __owner; } pthread_mutex_t;   /* [ref] 8 bytes */
typedef struct { int __count; unsigned __owner; } pthread_cond_t;    /* [ref] 8 bytes */
typedef struct { int __w[8]; } pthread_mutexattr_t;
typedef struct { int __w[8]; } pthread_condattr_t;
typedef struct { int __w[16]; } pthread_attr_t;
int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
int pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int pthread_cond_broadcast(pthread_cond_t *);
int pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int pthread_condattr_init(pthread_condattr_t *);
int pthread_condattr_destroy(pthread_condattr_t *);
int pthread_condattr_setclock(pthread_condattr_t *, clockid_t);
int pthread_sigmask(int, const sigset_t *, sigset_t *);
#endif
