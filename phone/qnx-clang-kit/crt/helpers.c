/* Private helpers for calls clang may generate that QNX 6.5 libc may not export.
 * Hidden so they are never exported from the plugins. */
#include <stddef.h>
int memcmp(const void *, const void *, size_t);
__attribute__((visibility("hidden"))) int bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }

/* 64-bit integer -> double (official builds link libgcc's versions statically).
 * hi * 2^32 is exact in a double, so the single addition gives a correctly
 * rounded result. Uses VFP conversions, so no recursion into these helpers. */
__attribute__((visibility("hidden"))) double __aeabi_ul2d(unsigned long long v)
{ return (double)(unsigned)(v >> 32) * 4294967296.0 + (double)(unsigned)v; }
__attribute__((visibility("hidden"))) double __aeabi_l2d(long long v)
{ return (double)(int)(v >> 32) * 4294967296.0 + (double)(unsigned)v; }
