// The settings registry at run time (docs/ARCHITECTURE.md, Settings; the rows are nxnative/dm/settings.tsv). build.py force-includes this header (-include) into every
// core source of the Android build, after darwin_compat.h. The sources keep plain getenv("DM_...") / setenv calls; in this build:
//   getenv("<literal>")  reads a per-call-site cache: the environment is searched again only after a setenv changed a value (a ctl.txt `env` line, the pause menu,
//                        env.txt at launch). The replay thread read about 50 switches at every submission, each a scan of the whole environment.
//   setenv / unsetenv    go through dm_setenv / dm_unsetenv: they bump the cache generation when a value changes, and log a [RED] line once for a DM_ name that has
//                        no row in settings.tsv (a typo in env.txt or a ctl line used to do nothing, silently).
// Test harnesses that cut blocks out of core sources do not include this header: there the calls stay plain getenv / setenv, with the same results.
#pragma once
#include <stdlib.h>   // before the macros below: its getenv / setenv declarations must not be rewritten
#include <stdatomic.h>

typedef struct dm_env_rec { _Atomic unsigned gen; const char *val; } dm_env_rec;
const char *dm_env_cached(dm_env_rec *_Atomic *slot, const char *name);   // a DM_ name's value through its call site's cache; other names read directly
int dm_setenv(const char *name, const char *value, int overwrite);
int dm_unsetenv(const char *name);
int dm_setting_known(const char *name);                                    // 1 when settings.tsv has a row for this name

#define getenv(n) (__builtin_constant_p(n) ? ({ static dm_env_rec *_Atomic dm_env_slot_; dm_env_cached(&dm_env_slot_, (n)); }) : (getenv)(n))
#define setenv(n, v, o) dm_setenv((n), (v), (o))
#define unsetenv(n) dm_unsetenv(n)
