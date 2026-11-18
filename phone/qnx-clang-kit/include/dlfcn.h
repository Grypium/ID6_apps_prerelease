/* QNX 6.5 dlopen. [ref] upstream's qcc-built worker passes 0x102 for RTLD_NOW|RTLD_GLOBAL. */
#ifndef QNX65_DLFCN_H
#define QNX65_DLFCN_H
#include "_qnx65.h"
#define RTLD_NOW    0x0002
#define RTLD_GLOBAL 0x0100
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
char *dlerror(void);
int dlclose(void *);
#endif
