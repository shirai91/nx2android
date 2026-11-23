// Android controller button remapping (front_android.c, settings.txt remap= rows).
// Maps physical Android keycodes, raw Linux scancodes (for extra buttons/paddles reporting
// AKEYCODE_UNKNOWN), and canonicalized trigger/HAT axes onto the 16 Switch NpadButton bits
// (dm_input.h DM_BTN_*). Standalone (no NDK header required) so host tests compile it directly.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dm_input.h"

#define PAD_BTN_N 16
#define PAD_SLOTS_N 2      // slot 0: Primary, slot 1: Secondary
#define PAD_HELD_MAX 32    // max simultaneously held physical inputs per player

// Canonical Android keycode numbers (ABI-frozen; match <android/keycodes.h>).
enum {
    PAD_KC_HOME = 3,
    PAD_KC_BACK = 4,
    PAD_KC_CALL = 5,
    PAD_KC_ENDCALL = 6,
    PAD_KC_0 = 7,
    PAD_KC_9 = 16,
    PAD_KC_DPAD_UP = 19,
    PAD_KC_DPAD_DOWN = 20,
    PAD_KC_DPAD_LEFT = 21,
    PAD_KC_DPAD_RIGHT = 22,
    PAD_KC_VOLUME_UP = 24,
    PAD_KC_VOLUME_DOWN = 25,
    PAD_KC_POWER = 26,
    PAD_KC_CAMERA = 27,
    PAD_KC_A = 29,
    PAD_KC_Z = 54,
    PAD_KC_HEADSETHOOK = 79,
    PAD_KC_FOCUS = 80,
    PAD_KC_MENU = 82,
    PAD_KC_NOTIFICATION = 83,
    PAD_KC_SEARCH = 84,
    PAD_KC_MEDIA_PLAY_PAUSE = 85,
    PAD_KC_MEDIA_STOP = 86,
    PAD_KC_MEDIA_NEXT = 87,
    PAD_KC_MEDIA_PREVIOUS = 88,
    PAD_KC_MEDIA_REWIND = 89,
    PAD_KC_MEDIA_FAST_FORWARD = 90,
    PAD_KC_MUTE = 91,
    PAD_KC_BTN_A = 96,
    PAD_KC_BTN_B = 97,
    PAD_KC_BTN_C = 98,
    PAD_KC_BTN_X = 99,
    PAD_KC_BTN_Y = 100,
    PAD_KC_BTN_Z = 101,
    PAD_KC_BTN_L1 = 102,
    PAD_KC_BTN_R1 = 103,
    PAD_KC_BTN_L2 = 104,
    PAD_KC_BTN_R2 = 105,
    PAD_KC_BTN_THUMBL = 106,
    PAD_KC_BTN_THUMBR = 107,
    PAD_KC_BTN_START = 108,
    PAD_KC_BTN_SELECT = 109,
    PAD_KC_BTN_MODE = 110,
    PAD_KC_MEDIA_PLAY = 126,
    PAD_KC_MEDIA_PAUSE = 127,
    PAD_KC_MEDIA_CLOSE = 128,
    PAD_KC_MEDIA_EJECT = 129,
    PAD_KC_MEDIA_RECORD = 130,
    PAD_KC_F1 = 131,
    PAD_KC_F12 = 142,
    PAD_KC_VOLUME_MUTE = 164,
    PAD_KC_APP_SWITCH = 187,
    PAD_KC_BTN_1 = 188,
    PAD_KC_BTN_2 = 189,
    PAD_KC_BTN_16 = 203,
    PAD_KC_ASSIST = 219,
    PAD_KC_BRIGHTNESS_DOWN = 220,
    PAD_KC_BRIGHTNESS_UP = 221,
    PAD_KC_SLEEP = 223,
    PAD_KC_WAKEUP = 224,
    PAD_KC_VOICE_ASSIST = 231,
};

// Physical input ID encoding:
//   0                 = unbound
//   1..0xFFFF         = Android AKeyEvent keyCode (also canonical target for analog L2/R2 and HAT D-pad)
//   0x10000..0x1FFFF  = raw Linux evdev scanCode when Android keyCode == AKEYCODE_UNKNOWN (0)
#define PAD_PHYS_SCAN_FLAG 0x10000u
#define PAD_PHYS_KEY(kc)   ((uint32_t)(kc) & 0xFFFFu)
#define PAD_PHYS_SCAN(sc)  (PAD_PHYS_SCAN_FLAG | ((uint32_t)(sc) & 0xFFFFu))
#define PAD_PHYS_IS_SCAN(p) (((p) & PAD_PHYS_SCAN_FLAG) != 0)
#define PAD_PHYS_VAL(p)    ((p) & 0xFFFFu)

typedef struct {
    int bit_idx;        // 0..15 (1ull << bit_idx == DM_BTN_*)
    const char *key;    // stable token in settings.txt
    const char *label;  // human-readable Switch button label in the pause menu
} pad_btn_desc;

// Display order in the Remap buttons UI (paged dynamically across OVM_ROWS).
static const pad_btn_desc PAD_MAP_ORDER[PAD_BTN_N] = {
    { 0, "a",     "A (Right)"     },
    { 1, "b",     "B (Bottom)"    },
    { 2, "x",     "X (Top)"       },
    { 3, "y",     "Y (Left)"      },
    {10, "plus",  "Plus (+)"      },
    {11, "minus", "Minus (-)"     },
    { 6, "l",     "L"             },
    { 7, "r",     "R"             },
    { 8, "zl",    "ZL"            },
    { 9, "zr",    "ZR"            },
    { 4, "ls",    "L3 (Stick L)"  },
    { 5, "rs",    "R3 (Stick R)"  },
    {13, "up",    "D-Pad Up"      },
    {15, "down",  "D-Pad Down"    },
    {12, "left",  "D-Pad Left"    },
    {14, "right", "D-Pad Right"   },
};

typedef struct {
    int custom;                                // 0: follow factory defaults + pad_labels(); 1: use binds[][]
    uint32_t binds[PAD_BTN_N][PAD_SLOTS_N];    // indexed by Switch bit_idx (0..15)
} pad_remap_table;

typedef struct {
    uint32_t keys[PAD_HELD_MAX];
    int n;
} pad_held_keys;

static inline int pad_remap_key_blocked(int32_t k) {
    switch (k) {
    case PAD_KC_HOME: case PAD_KC_BACK: case PAD_KC_CALL: case PAD_KC_ENDCALL:
    case PAD_KC_VOLUME_UP: case PAD_KC_VOLUME_DOWN: case PAD_KC_POWER: case PAD_KC_CAMERA:
    case PAD_KC_HEADSETHOOK: case PAD_KC_FOCUS: case PAD_KC_MENU: case PAD_KC_NOTIFICATION:
    case PAD_KC_SEARCH: case PAD_KC_MEDIA_PLAY_PAUSE: case PAD_KC_MEDIA_STOP:
    case PAD_KC_MEDIA_NEXT: case PAD_KC_MEDIA_PREVIOUS: case PAD_KC_MEDIA_REWIND:
    case PAD_KC_MEDIA_FAST_FORWARD: case PAD_KC_MUTE: case PAD_KC_BTN_MODE:
    case PAD_KC_MEDIA_PLAY: case PAD_KC_MEDIA_PAUSE: case PAD_KC_MEDIA_CLOSE:
    case PAD_KC_MEDIA_EJECT: case PAD_KC_MEDIA_RECORD: case PAD_KC_VOLUME_MUTE:
    case PAD_KC_APP_SWITCH: case PAD_KC_ASSIST: case PAD_KC_BRIGHTNESS_DOWN:
    case PAD_KC_BRIGHTNESS_UP: case PAD_KC_SLEEP: case PAD_KC_WAKEUP: case PAD_KC_VOICE_ASSIST:
        return 1;
    default:
        return 0;
    }
}

static inline uint32_t pad_phys_from_key(int32_t keycode, int32_t scancode) {
    if (keycode > 0 && keycode <= 0xFFFF) return pad_remap_key_blocked(keycode) ? 0u : PAD_PHYS_KEY(keycode);
    if (keycode == 0 && scancode > 0 && scancode <= 0xFFFF) return PAD_PHYS_SCAN(scancode);
    return 0u;
}

static inline uint64_t pad_remap_default_bit(uint32_t phys, int labels) {
    if (!phys || PAD_PHYS_IS_SCAN(phys)) return 0;
    if (labels) {
        switch (phys) {
        case PAD_KC_BTN_A: return DM_BTN_A;
        case PAD_KC_BTN_B: return DM_BTN_B;
        case PAD_KC_BTN_X: return DM_BTN_X;
        case PAD_KC_BTN_Y: return DM_BTN_Y;
        default: break;
        }
    }
    switch (phys) {
    case PAD_KC_BTN_A:      return DM_BTN_B;
    case PAD_KC_BTN_B:      return DM_BTN_A;
    case PAD_KC_BTN_X:      return DM_BTN_Y;
    case PAD_KC_BTN_Y:      return DM_BTN_X;
    case PAD_KC_BTN_L1:     return DM_BTN_L;
    case PAD_KC_BTN_R1:     return DM_BTN_R;
    case PAD_KC_BTN_L2:     return DM_BTN_ZL;
    case PAD_KC_BTN_R2:     return DM_BTN_ZR;
    case PAD_KC_BTN_THUMBL: return DM_BTN_LS;
    case PAD_KC_BTN_THUMBR: return DM_BTN_RS;
    case PAD_KC_BTN_START:  return DM_BTN_PLUS;
    case PAD_KC_BTN_SELECT: return DM_BTN_MINUS;
    case PAD_KC_DPAD_UP:    return DM_BTN_UP;
    case PAD_KC_DPAD_DOWN:  return DM_BTN_DOWN;
    case PAD_KC_DPAD_LEFT:  return DM_BTN_LEFT;
    case PAD_KC_DPAD_RIGHT: return DM_BTN_RIGHT;
    default: return 0;
    }
}

static inline void pad_remap_fill_defaults(pad_remap_table *t, int labels) {
    memset(t, 0, sizeof *t);
    t->binds[0][0]  = labels ? PAD_KC_BTN_A : PAD_KC_BTN_B;   // DM_BTN_A
    t->binds[1][0]  = labels ? PAD_KC_BTN_B : PAD_KC_BTN_A;   // DM_BTN_B
    t->binds[2][0]  = labels ? PAD_KC_BTN_X : PAD_KC_BTN_Y;   // DM_BTN_X
    t->binds[3][0]  = labels ? PAD_KC_BTN_Y : PAD_KC_BTN_X;   // DM_BTN_Y
    t->binds[4][0]  = PAD_KC_BTN_THUMBL;                      // DM_BTN_LS
    t->binds[5][0]  = PAD_KC_BTN_THUMBR;                      // DM_BTN_RS
    t->binds[6][0]  = PAD_KC_BTN_L1;                          // DM_BTN_L
    t->binds[7][0]  = PAD_KC_BTN_R1;                          // DM_BTN_R
    t->binds[8][0]  = PAD_KC_BTN_L2;                          // DM_BTN_ZL
    t->binds[9][0]  = PAD_KC_BTN_R2;                          // DM_BTN_ZR
    t->binds[10][0] = PAD_KC_BTN_START;                       // DM_BTN_PLUS
    t->binds[11][0] = PAD_KC_BTN_SELECT;                      // DM_BTN_MINUS
    t->binds[12][0] = PAD_KC_DPAD_LEFT;                       // DM_BTN_LEFT
    t->binds[13][0] = PAD_KC_DPAD_UP;                         // DM_BTN_UP
    t->binds[14][0] = PAD_KC_DPAD_RIGHT;                      // DM_BTN_RIGHT
    t->binds[15][0] = PAD_KC_DPAD_DOWN;                       // DM_BTN_DOWN
}

static inline void pad_remap_ensure_custom(pad_remap_table *t, int labels) {
    if (!t->custom) {
        pad_remap_fill_defaults(t, labels);
        t->custom = 1;
    }
}

static inline uint32_t pad_remap_slot(const pad_remap_table *t, int labels, int btn_idx, int slot) {
    if (btn_idx < 0 || btn_idx >= PAD_BTN_N || slot < 0 || slot >= PAD_SLOTS_N) return 0;
    if (t->custom) return t->binds[btn_idx][slot];
    if (slot != 0) return 0;
    pad_remap_table d;
    pad_remap_fill_defaults(&d, labels);
    return d.binds[btn_idx][0];
}

static inline void pad_remap_set(pad_remap_table *t, int labels, int btn_idx, int slot, uint32_t phys) {
    if (btn_idx < 0 || btn_idx >= PAD_BTN_N || slot < 0 || slot >= PAD_SLOTS_N) return;
    pad_remap_ensure_custom(t, labels);
    if (phys) {
        for (int b = 0; b < PAD_BTN_N; b++)
            for (int s = 0; s < PAD_SLOTS_N; s++)
                if (t->binds[b][s] == phys) t->binds[b][s] = 0;
    }
    t->binds[btn_idx][slot] = phys;
}

static inline void pad_remap_clear(pad_remap_table *t, int labels, int btn_idx, int slot) {
    if (btn_idx < 0 || btn_idx >= PAD_BTN_N || slot < 0 || slot >= PAD_SLOTS_N) return;
    pad_remap_ensure_custom(t, labels);
    t->binds[btn_idx][slot] = 0;
}

static inline void pad_remap_reset(pad_remap_table *t, int labels) {
    pad_remap_fill_defaults(t, labels);
}

// When the player toggles Face buttons (Position <-> Label) while custom binds are active,
// swap A<->B and X<->Y physical codes in place so secondary bindings stay intact.
static inline void pad_remap_flip_face_labels(pad_remap_table *t) {
    if (!t->custom) return;
    for (int b = 0; b < PAD_BTN_N; b++) {
        for (int s = 0; s < PAD_SLOTS_N; s++) {
            uint32_t p = t->binds[b][s];
            if (p == PAD_KC_BTN_A) t->binds[b][s] = PAD_KC_BTN_B;
            else if (p == PAD_KC_BTN_B) t->binds[b][s] = PAD_KC_BTN_A;
            else if (p == PAD_KC_BTN_X) t->binds[b][s] = PAD_KC_BTN_Y;
            else if (p == PAD_KC_BTN_Y) t->binds[b][s] = PAD_KC_BTN_X;
        }
    }
}

static inline uint64_t pad_remap_eval(const pad_remap_table *t, int labels, uint32_t phys) {
    if (!phys) return 0;
    if (!t->custom) return pad_remap_default_bit(phys, labels);
    uint64_t out = 0;
    for (int b = 0; b < PAD_BTN_N; b++) {
        if (t->binds[b][0] == phys || t->binds[b][1] == phys) out |= 1ull << b;
    }
    return out;
}

static inline uint64_t pad_remap_axes_eval(const float axes[8], const pad_remap_table *t, int labels) {
    uint64_t out = 0;
    if (axes[4] > 0.5f)  out |= pad_remap_eval(t, labels, PAD_KC_BTN_L2);
    if (axes[5] > 0.5f)  out |= pad_remap_eval(t, labels, PAD_KC_BTN_R2);
    if (axes[6] < -0.5f) out |= pad_remap_eval(t, labels, PAD_KC_DPAD_LEFT);
    if (axes[6] > 0.5f)  out |= pad_remap_eval(t, labels, PAD_KC_DPAD_RIGHT);
    if (axes[7] < -0.5f) out |= pad_remap_eval(t, labels, PAD_KC_DPAD_UP);
    if (axes[7] > 0.5f)  out |= pad_remap_eval(t, labels, PAD_KC_DPAD_DOWN);
    return out;
}

static inline void pad_held_update(pad_held_keys *h, uint32_t phys, int down, int up) {
    if (!phys) return;
    if (down) {
        for (int i = 0; i < h->n; i++) if (h->keys[i] == phys) return;
        if (h->n < PAD_HELD_MAX) h->keys[h->n++] = phys;
    } else if (up) {
        for (int i = 0; i < h->n; i++) {
            if (h->keys[i] == phys) { h->keys[i] = h->keys[--h->n]; return; }
        }
    }
}

static inline uint64_t pad_held_eval(const pad_held_keys *h, const pad_remap_table *t, int labels) {
    uint64_t out = 0;
    for (int i = 0; i < h->n; i++) out |= pad_remap_eval(t, labels, h->keys[i]);
    return out;
}

static inline int pad_btn_by_key(const char *name) {
    for (int i = 0; i < PAD_BTN_N; i++) if (!strcmp(PAD_MAP_ORDER[i].key, name)) return PAD_MAP_ORDER[i].bit_idx;
    return -1;
}

static inline const char *pad_btn_key_name(int bit_idx) {
    for (int i = 0; i < PAD_BTN_N; i++) if (PAD_MAP_ORDER[i].bit_idx == bit_idx) return PAD_MAP_ORDER[i].key;
    return NULL;
}

static inline int pad_remap_parse(pad_remap_table *t, const char *line) {
    int v;
    if (sscanf(line, "remap_custom=%d", &v) == 1) {
        if (v) { memset(t->binds, 0, sizeof t->binds); t->custom = 1; }
        else { t->custom = 0; }
        return 1;
    }
    if (!strncmp(line, "remap=", 6)) {
        char nm[16]; unsigned s0 = 0, s1 = 0;
        if (sscanf(line + 6, "%15[^,],%x,%x", nm, &s0, &s1) >= 2) {
            int b = pad_btn_by_key(nm);
            if (b >= 0) {
                t->custom = 1;
                t->binds[b][0] = s0 & 0x1FFFFu;
                t->binds[b][1] = s1 & 0x1FFFFu;
            }
        }
        return 1;
    }
    return 0;
}

static inline void pad_remap_write(const pad_remap_table *t, FILE *f) {
    if (!t->custom) return;
    fprintf(f, "remap_custom=1\n");
    for (int i = 0; i < PAD_BTN_N; i++) {
        int b = PAD_MAP_ORDER[i].bit_idx;
        fprintf(f, "remap=%s,%x,%x\n", PAD_MAP_ORDER[i].key, (unsigned)t->binds[b][0], (unsigned)t->binds[b][1]);
    }
}

static inline void pad_phys_name(uint32_t phys, char *out, size_t cap) {
    if (!phys) { snprintf(out, cap, "None"); return; }
    if (PAD_PHYS_IS_SCAN(phys)) { snprintf(out, cap, "Scan %u", (unsigned)PAD_PHYS_VAL(phys)); return; }
    switch (phys) {
    case PAD_KC_BTN_A:      snprintf(out, cap, "BTN A"); return;
    case PAD_KC_BTN_B:      snprintf(out, cap, "BTN B"); return;
    case PAD_KC_BTN_C:      snprintf(out, cap, "BTN C"); return;
    case PAD_KC_BTN_X:      snprintf(out, cap, "BTN X"); return;
    case PAD_KC_BTN_Y:      snprintf(out, cap, "BTN Y"); return;
    case PAD_KC_BTN_Z:      snprintf(out, cap, "BTN Z"); return;
    case PAD_KC_BTN_L1:     snprintf(out, cap, "L1"); return;
    case PAD_KC_BTN_R1:     snprintf(out, cap, "R1"); return;
    case PAD_KC_BTN_L2:     snprintf(out, cap, "L2"); return;
    case PAD_KC_BTN_R2:     snprintf(out, cap, "R2"); return;
    case PAD_KC_BTN_THUMBL: snprintf(out, cap, "L3"); return;
    case PAD_KC_BTN_THUMBR: snprintf(out, cap, "R3"); return;
    case PAD_KC_BTN_START:  snprintf(out, cap, "Start"); return;
    case PAD_KC_BTN_SELECT: snprintf(out, cap, "Select"); return;
    case PAD_KC_DPAD_UP:    snprintf(out, cap, "D-Up"); return;
    case PAD_KC_DPAD_DOWN:  snprintf(out, cap, "D-Down"); return;
    case PAD_KC_DPAD_LEFT:  snprintf(out, cap, "D-Left"); return;
    case PAD_KC_DPAD_RIGHT: snprintf(out, cap, "D-Right"); return;
    default: break;
    }
    if (phys >= PAD_KC_BTN_1 && phys <= PAD_KC_BTN_16) {
        snprintf(out, cap, "BTN %u", (unsigned)(phys - PAD_KC_BTN_1 + 1));
        return;
    }
    if (phys >= PAD_KC_F1 && phys <= PAD_KC_F12) {
        snprintf(out, cap, "F%u", (unsigned)(phys - PAD_KC_F1 + 1));
        return;
    }
    if (phys >= PAD_KC_A && phys <= PAD_KC_Z) {
        snprintf(out, cap, "Key %c", (char)('A' + (phys - PAD_KC_A)));
        return;
    }
    if (phys >= PAD_KC_0 && phys <= PAD_KC_9) {
        snprintf(out, cap, "Key %c", (char)('0' + (phys - PAD_KC_0)));
        return;
    }
    snprintf(out, cap, "Key %u", (unsigned)phys);
}

static inline void pad_remap_summary(const pad_remap_table *t, int labels, int btn_idx, char *out, size_t cap) {
    uint32_t s0 = pad_remap_slot(t, labels, btn_idx, 0);
    uint32_t s1 = pad_remap_slot(t, labels, btn_idx, 1);
    char n0[20], n1[20];
    if (s0 && s1) {
        pad_phys_name(s0, n0, sizeof n0);
        pad_phys_name(s1, n1, sizeof n1);
        snprintf(out, cap, "%s + %s", n0, n1);
    } else if (s0) {
        pad_phys_name(s0, out, cap);
    } else if (s1) {
        pad_phys_name(s1, out, cap);
    } else {
        snprintf(out, cap, "None");
    }
}
