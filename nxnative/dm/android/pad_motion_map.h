// A controller's own axes, as the host's six-axis sensor reads them (front_android.c pad_motion_feed, tests/test_android_motion.py)
#pragma once

// A pad's sensors come in Android's frame for that device (x to its right, y up, z out of its face), and the frame is not measured on this path: a HID driver and the input
// stack's sensor mapper sit between the pad and InputDevice.getSensorManager, and a pad that arrives permuted or negated aims sideways with nothing to say why. The spec is
// one signed axis per output axis - "-y,+x,+z" is output 0 = minus input y, output 1 = +input x, output 2 = +input z - which is what a Flydigi Direwolf 4 in Switch mode
// needs (measured on a phone: it puts x toward the top edge and y to the left). The accelerometer and the gyroscope share the pad's frame, so one spec covers both.
//
// A spec that is not a signed permutation of x, y and z is refused whole, not half applied: a typo then leaves the pad as Android reports it, a frame a player can still
// recognise, instead of a frame that is wrong in a way nothing logs. The same rule keeps an empty DM_PAD_MOTION_MAP (the default) equal to identity.
typedef struct {
    int on;          // 0: the values go through as Android reports them (identity)
    int idx[3];      // output axis i is input axis idx[i]
    float sign[3];   // and carries this sign
} dm_pad_map;

static inline dm_pad_map dm_pad_map_identity(void) {
    dm_pad_map m;
    m.on = 0;
    m.idx[0] = 0; m.idx[1] = 1; m.idx[2] = 2;
    m.sign[0] = m.sign[1] = m.sign[2] = 1;
    return m;
}

// 1 when the spec was taken and `out` carries it; 0 leaves `out` alone (call it with an identity map: the pad's own frame is then used as Android reports it)
static inline int dm_pad_map_parse(const char *spec, dm_pad_map *out) {
    if (!spec || !*spec) return 0;
    dm_pad_map m = dm_pad_map_identity();
    int seen = 0;
    const char *p = spec;
    for (int i = 0; i < 3; i++) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        float s = 1;
        if (*p == '-') { s = -1; p++; } else if (*p == '+') { p++; }
        char c = *p;
        int a = c == 'x' || c == 'X' ? 0 : c == 'y' || c == 'Y' ? 1 : c == 'z' || c == 'Z' ? 2 : -1;
        if (a < 0 || (seen & (1 << a))) return 0;   // not an axis, or the same one twice: no permutation
        seen |= 1 << a;
        m.idx[i] = a; m.sign[i] = s;
        p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return 0;   // more than three axes
    m.on = 1;
    *out = m;
    return 1;
}

// output i = sign[i] * input[idx[i]]; an identity map leaves the values untouched
static inline void dm_pad_map_apply(const dm_pad_map *m, float v[3]) {
    if (!m || !m->on) return;
    float in[3] = {v[0], v[1], v[2]};
    for (int i = 0; i < 3; i++) v[i] = m->sign[i] * in[m->idx[i]];
}
