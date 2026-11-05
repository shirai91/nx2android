// Android frame-limit choices. Zero restores the environment/profile default.
#pragma once
static const int DM_FRAME_LIMITS[] = {0, 30, 40, 50, 60, 90, 120};
#define DM_FRAME_LIMIT_COUNT ((int)(sizeof DM_FRAME_LIMITS / sizeof *DM_FRAME_LIMITS))
static inline int dm_frame_limit_index(int fps) {
    for (int i = 0; i < DM_FRAME_LIMIT_COUNT; i++) if (DM_FRAME_LIMITS[i] == fps) return i;
    return -1;
}
static inline int dm_frame_limit_valid(int fps) { return dm_frame_limit_index(fps) >= 0; }
static inline int dm_frame_limit_step(int fps, int direction, int wrap) {
    int i = dm_frame_limit_index(fps); if (i < 0) i = 0;
    i += direction < 0 ? -1 : 1;
    if (wrap) i = (i + DM_FRAME_LIMIT_COUNT) % DM_FRAME_LIMIT_COUNT;
    else { if (i < 0) i = 0; if (i >= DM_FRAME_LIMIT_COUNT) i = DM_FRAME_LIMIT_COUNT - 1; }
    return DM_FRAME_LIMITS[i];
}
