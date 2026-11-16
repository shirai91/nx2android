// Android port: clonefile = a recursive copy (no copy-on-write clones on ext4 / f2fs; save directories are small), renamex_np(RENAME_SWAP) =
// renameat2(RENAME_EXCHANGE), which swaps two directory entries atomically as APFS does
#pragma once
#include <stdint.h>
#define CLONE_NOFOLLOW 0x1
#define RENAME_SWAP 0x2
#define RENAME_EXCL 0x4
int clonefile(const char *src, const char *dst, uint32_t flags);
int renamex_np(const char *from, const char *to, unsigned flags);
