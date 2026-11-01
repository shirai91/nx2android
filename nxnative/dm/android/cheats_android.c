// Android port: upstream's cheats engine (host/hle_cheats.c) writes its state files on a GCD queue with blocks, which bionic has neither of, and is not taken. The host runs without
// cheats: these are the calls the shared host makes (main.c, hle_lifecycle.c, hle_nvn.c's present)
#include "dm.h"
void dm_cheats_init(void) {}
void dm_cheats_present(void) {}
void dm_cheats_exit(void) {}
