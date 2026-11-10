#pragma once
#include <stddef.h>
#include <stdint.h>

// The reference scan over host-lent command segments (hle_nvn.c hostseg_scan): a word of memory that points into the arena marks the segment it points into.
// pages holds, per 4 KB page of the arena, the segment's index + 1 (0 = no segment there). self: the segment these words are in, whose pointers to itself keep nothing alive (-1 = none)
static inline void hostseg_mark_words(const uint64_t *w, size_t n, uint64_t base, uint64_t span, const int32_t *pages, int self, uint8_t *mark) {
    for (size_t i = 0; i < n; i++) {
        uint64_t v = w[i] - base;
        if (v >= span) continue;
        int at = pages[v >> 12] - 1;
        if (at >= 0 && at != self) mark[at] = 1;
    }
}
