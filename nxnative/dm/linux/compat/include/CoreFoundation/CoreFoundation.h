// Android port: the one CoreFoundation use, a mutable pointer-keyed dictionary with no callbacks (identity keys, values not retained),
// as an open-addressing hash table (dm/android/compat/darwin_compat.c)
#pragma once
#include <stddef.h>
typedef struct dm_cfdict *CFMutableDictionaryRef, *CFDictionaryRef;
typedef const void *CFAllocatorRef;
typedef long CFIndex;
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *kcb, const void *vcb);
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *key);
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *key, const void *value);
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *key);
CFIndex CFDictionaryGetCount(CFDictionaryRef d);
