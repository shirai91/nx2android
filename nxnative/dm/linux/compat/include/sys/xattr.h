// Android port: Darwin's extended attribute calls (a position and an options argument; XATTR_NOFOLLOW) on Linux's (l*xattr for no-follow).
// Linux needs a namespace in the name: a Darwin-style name without one goes into "user.".
#pragma once
#include_next <sys/xattr.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define XATTR_NOFOLLOW 0x0001
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
static inline const char *dm_xattr_name(const char *n, char *buf, size_t cap) {
    if (!strncmp(n, "user.", 5) || !strncmp(n, "trusted.", 8) || !strncmp(n, "security.", 9) || !strncmp(n, "system.", 7)) return n;
    snprintf(buf, cap, "user.%s", n); return buf;
}
static inline ssize_t dm_getxattr(const char *p, const char *n, void *v, size_t s, uint32_t pos, int opt) {
    (void)pos; char b[256]; n = dm_xattr_name(n, b, sizeof b); return opt & XATTR_NOFOLLOW ? lgetxattr(p, n, v, s) : getxattr(p, n, v, s);
}
static inline int dm_setxattr(const char *p, const char *n, const void *v, size_t s, uint32_t pos, int opt) {
    (void)pos; char b[256]; n = dm_xattr_name(n, b, sizeof b); return opt & XATTR_NOFOLLOW ? lsetxattr(p, n, v, s, 0) : setxattr(p, n, v, s, 0);
}
static inline int dm_removexattr(const char *p, const char *n, int opt) {
    char b[256]; n = dm_xattr_name(n, b, sizeof b); return opt & XATTR_NOFOLLOW ? lremovexattr(p, n) : removexattr(p, n);
}
#define getxattr(p, n, v, s, pos, opt) dm_getxattr(p, n, v, s, pos, opt)
#define setxattr(p, n, v, s, pos, opt) dm_setxattr(p, n, v, s, pos, opt)
#define removexattr(p, n, opt) dm_removexattr(p, n, opt)
