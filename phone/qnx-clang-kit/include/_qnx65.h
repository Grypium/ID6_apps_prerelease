/*
 * Minimal QNX Neutrino 6.5.0 (armle-v7) ABI declarations for building the
 * NBT Evo apps with an open-source compiler. Written from scratch.
 *
 * Every constant/layout marked [ref] was read out of the official qcc-built
 * binaries (old release payload) by disassembly. [doc] = taken from public
 * QNX documentation / BSD-derived values and NOT confirmed in the reference.
 */
#ifndef QNX65_COMPAT_H
#define QNX65_COMPAT_H

#include <stddef.h>   /* clang builtin */
#include <stdint.h>   /* clang builtin */
#include <stdarg.h>   /* clang builtin */

#ifndef __QNX__
#define __QNX__ 1
#endif
#ifndef __QNXNTO__
#define __QNXNTO__ 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- basic types (32-bit ARM, QNX _INT32 style) ---- */
typedef int ssize_t;
typedef int pid_t;
typedef int uid_t;
typedef int gid_t;
typedef long off_t;            /* 32-bit, no _FILE_OFFSET_BITS=64 */
typedef unsigned int mode_t;
typedef unsigned int socklen_t;
typedef long time_t;
typedef int clockid_t;
typedef int sig_atomic_t;
typedef uint8_t sa_family_t;   /* [ref] family stored as a byte */
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;
typedef struct { long __bits[2]; } sigset_t;   /* [ref] 8 bytes: sigmask@+8, sigdefault@+16 */

/* ---- errno ---- [ref] code reads errno through __get_errno_ptr() */
int *__get_errno_ptr(void);
#define errno (*__get_errno_ptr())
#define ENOENT        2    /* [ref] fileops */
#define EINTR         4    /* [ref] */
#define ECHILD       10    /* [ref] */
#define EAGAIN       11    /* [ref] */
#define EWOULDBLOCK  EAGAIN
#define ERANGE       34    /* [ref] */
#define EINPROGRESS 236    /* [ref] */
#define EADDRINUSE  248    /* [doc] only selects a message string */
#define ETIMEDOUT   260    /* [ref] upstream MOST worker compares pthread_cond_timedwait with 260 */

/* ---- stdio ---- [ref] stderr is &_Stderr (80-byte FILE, copy-relocated) */
typedef struct _Filet { unsigned char __opaque[80]; } FILE;
extern FILE _Stderr;
#define stderr (&_Stderr)
FILE *fopen(const char *, const char *);
int fclose(FILE *);
char *fgets(char *, int, FILE *);
int fgetc(FILE *);
int fprintf(FILE *, const char *, ...);
int vfprintf(FILE *, const char *, va_list);
int fputc(int, FILE *);
int printf(const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int puts(const char *);
size_t fwrite(const void *, size_t, size_t, FILE *);
void perror(const char *);
int fputs(const char *, FILE *);
#define EOF (-1)

/* ---- string ---- */
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memchr(const void *, int, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strcpy(char *, const char *);
char *strrchr(const char *, int);
char *strchr(const char *, int);
void *memmove(void *, const void *, unsigned int);
size_t strcspn(const char *, const char *);
char *strerror(int);

/* ---- stdlib ---- */
double strtod(const char *, char **);
unsigned long strtoul(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
void exit(int) __attribute__((noreturn));
int atexit(void (*)(void));

/* ---- ctype ----
 * QNX's macros index the _Ctype table; we use equivalent inline ASCII tests
 * ("C" locale) so no table import is needed. */
static inline int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int isprint(int c) { return c >= 0x20 && c < 0x7f; }
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int toupper(int c) { return islower(c) ? c - 32 : c; }
static inline int tolower(int c) { return isupper(c) ? c + 32 : c; }

/* ---- math ---- [ref] sin(x) compiles to _Sin(x, 0) (Dinkumware) */
double _Sin(double, unsigned int);
#define sin(x) _Sin((x), 0)
#define cos(x) _Sin((x), 1)
#define fabs(x) __builtin_fabs(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnan(x) __builtin_isnan(x)
#define INFINITY __builtin_inf()
#define NAN __builtin_nan("")

/* ---- time ---- */
struct timespec { time_t tv_sec; long tv_nsec; };
#define CLOCK_REALTIME  0   /* [doc] */
#define CLOCK_MONOTONIC 2   /* [ref] */
int clock_gettime(clockid_t, struct timespec *);

/* ---- unistd / fcntl ---- */
int close(int);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
int pipe(int[2]);
pid_t getppid(void);
int rmdir(const char *);
int open(const char *, int, ...);
int fcntl(int, int, ...);
#define O_RDONLY   0       /* [doc] */
#define O_WRONLY   1       /* [doc] */
#define O_RDWR     2       /* [ref] */
#define O_NONBLOCK 0x80    /* [ref] */
#define F_SETFD    2       /* [ref] */
#define F_SETFL    4       /* [ref] */
#define FD_CLOEXEC 1       /* [ref] */

/* ---- stat (fileops helper only) ----
 * Layout not yet confirmed; fileops only tests lstat()'s return value. */
struct stat { unsigned char __opaque[128]; };
int lstat(const char *, struct stat *);
int rename(const char *, const char *);

/* ---- signals ---- */
#define SIGHUP   1    /* [ref] MOST worker pthread_sigmask set */
#define SIGINT   2    /* [ref] */
#define SIGPIPE 13    /* [ref] */
#define SIGALRM 14    /* [ref] upstream diag_link_signals */
#define SIGTERM 15    /* [ref] */
#define SIG_IGN ((void (*)(int))1)  /* [ref] */
#define SIG_DFL ((void (*)(int))0)  /* [doc] */
void (*signal(int, void (*)(int)))(int);
int kill(pid_t, int);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sigaddset(sigset_t *, int);
/* [ref] upstream's qcc-built worker: 16 bytes, handler@0, flags@4, mask@8 */
struct sigaction {
    void (*sa_handler)(int);
    int sa_flags;
    sigset_t sa_mask;
};
int sigaction(int, const struct sigaction *, struct sigaction *);
#define SIG_BLOCK   0   /* [ref] */
#define SIG_UNBLOCK 1   /* [ref] */
#define SIG_SETMASK 2   /* [doc] */
int sigprocmask(int, const sigset_t *, sigset_t *);
unsigned alarm(unsigned);

/* ---- wait ---- */
#define WNOHANG 0x40  /* [ref] */
pid_t waitpid(pid_t, int *, int);

/* ---- QNX spawn() ---- [ref] layout: flags@0, sigmask@8, sigdefault@16, size 88 */
struct inheritance {
    unsigned long flags;
    pid_t pgroup;
    sigset_t sigmask;
    sigset_t sigdefault;
    unsigned char __rest[88 - 24];
};
#define SPAWN_SETSIGMASK 0x2  /* [ref] flags==6 for SETSIGDEF|SETSIGMASK */
#define SPAWN_SETSIGDEF  0x4
pid_t spawn(const char *, int, const int[], const struct inheritance *,
            char *const[], char *const[]);

/* ---- poll ---- [ref] */
struct pollfd { int fd; short events; short revents; };
#define POLLIN  5
#define POLLOUT 2
int poll(struct pollfd *, unsigned int, int);

/* ---- sockets ---- [ref] BSD layout (sin_len byte, then family byte) */
struct sockaddr { uint8_t sa_len; sa_family_t sa_family; char sa_data[14]; };
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in {
    uint8_t sin_len; sa_family_t sin_family; in_port_t sin_port;
    struct in_addr sin_addr; char sin_zero[8];
};
#define AF_INET     2        /* [ref] */
#define PF_INET     AF_INET
#define SOCK_STREAM 1        /* [ref] */
#define SOCK_DGRAM  2        /* [ref] */
#define SOL_SOCKET  0xffff   /* [ref] */
#define SO_ERROR    0x1007   /* [ref] */
#define SO_BROADCAST 0x20    /* [doc] */
#define SO_REUSEADDR 0x4     /* [doc] */
#define IPPROTO_TCP 6
#define TCP_NODELAY 1        /* [doc] */
#define INADDR_ANY       ((in_addr_t)0)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define htons(x) __builtin_bswap16((uint16_t)(x))
#define ntohs(x) __builtin_bswap16((uint16_t)(x))
#define htonl(x) __builtin_bswap32((uint32_t)(x))
#define ntohl(x) __builtin_bswap32((uint32_t)(x))
int socket(int, int, int);
int bind(int, const struct sockaddr *, socklen_t);
int connect(int, const struct sockaddr *, socklen_t);
ssize_t send(int, const void *, size_t, int);
ssize_t sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
ssize_t recv(int, void *, size_t, int);
int getsockopt(int, int, int, void *, socklen_t *);
int setsockopt(int, int, int, const void *, socklen_t);
int inet_pton(int, const char *, void *);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
/* GCC 4.4 libstdc++ (COW) std::string: one pointer, mangles as "Ss".
 * The apps only name it in a virtual-method signature. */
namespace std {
template <class C> struct char_traits;
template <class T> class allocator;
template <class C, class T, class A> class basic_string { void *_M_p; };
typedef basic_string<char, char_traits<char>, allocator<char> > string;
}
#endif

#endif
