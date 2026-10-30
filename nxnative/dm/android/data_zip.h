// The game data's zip (front_android.c, the pause menu's game data page), laid out as Eden exports a save: <title ID>/ with the save's files, <title ID>_device/ the device save's
#pragma once
#include <stddef.h>
#include <stdint.h>

int dz_write(int fd, const char *const dirs[], const char *const names[], int n, unsigned *files);   // each dirs[k] as names[k]/...: 0, or -1
int dz_extract(const uint8_t *b, size_t n, const char *const dest[2]);   // into dest[0] (and dest[1], a <...>_device folder's): the files, or -1
