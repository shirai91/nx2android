// A controller stick's value for the game (front_android.c pad_publish, tests/test_stick_axis.py)
#pragma once
#include <math.h>
#include <stdint.h>
#include "dm_input.h"

// One axis: nothing inside the dead zone, else -1..1 scaled to the Switch's stick range. The value is clamped first: DM_STICK_MAX is the int16 limit and the host packs the axes
// into 16 bits, so 1.2 (39,320) arrived as -26,216, the opposite direction
static inline int32_t dm_stick_axis(float v) {
    if (v > -0.12f && v < 0.12f) return 0;
    v = v > 1 ? 1 : v < -1 ? -1 : v;
    return (int32_t)(v * DM_STICK_MAX);
}
// A Switch stick's gate is round (magnitude <= 1). A handheld's square-gated stick reports corner values up to 1.41 (measured on the AYN Thor in another port of this host, with
// Persona 4 Golden: 42% of the moving samples above 1.05, at most 1.35), and clamping each axis alone passes a corner as (1, 1): a diagonal faster than the game expects, and a push along an edge that turns into the diagonal. Both axes
// are scaled down together to magnitude 1 first
static inline void dm_stick_xy(float x, float y, int32_t *ox, int32_t *oy) {
    float m = hypotf(x, y);
    if (m > 1.0f) { x /= m; y /= m; }
    *ox = dm_stick_axis(x); *oy = dm_stick_axis(y);
}
