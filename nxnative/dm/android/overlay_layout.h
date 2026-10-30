// Android port: the on-screen gamepad's layout, shared by the touch handling (front_android.c) and its drawing (gfx_vk_present.c, with the app's theme: overlay_theme.c).
// Two layouts, the tablet's and the phone's (dm/android/overlay_default.tsv): a control's centre is x window heights from its anchor (the left edge, the middle
// or the right edge: a thumb reaches as far from the edge on any width) and y of the window's height down; its size is in window heights (round controls stay round)
#pragma once
#include <math.h>
#include "dm_input.h"
enum { OV_STICK_L, OV_STICK_R, OV_BUTTON, OV_DPAD, OV_MENU };   // OV_MENU: the menu button (always shown, 70%): pauses the game and opens the settings
typedef struct { int kind; float ax, x, y, w, h; uint64_t btn; const char *label, *name; } ov_ctl;   // ax: the anchor, as a fraction of the window's width; name: the table's
// The layouts: dm/android/overlay_default.tsv with the game profile's moves (android.tsv overlay rows), compiled by build.py into android_profile.h. Both list the
// same controls in the same order, so a control's index means the same in either (a press keeps it across a change of layout)
#include "android_profile.h"
static const ov_ctl OV_LAYOUTS[2][DM_AOV_N] = {{DM_AOV_TABLET}, {DM_AOV_PHONE}};
#define OV_N DM_AOV_N
// the layout in use: the phone's when the window is at least OV_PHONE_ASPECT wide for its height (a 20:9 phone is 2.2, a 16:10 tablet 1.6), else the tablet's
#define OV_PHONE_ASPECT 1.9f
int dm_overlay_layout(void);   // front_android.c: 0 the tablet's, 1 the phone's
#define OV_CTLS (OV_LAYOUTS[dm_overlay_layout()])
#define OV_KNOB 0.42f        // a stick's knob radius, of the stick's (the theme-less drawing)
#define OV_MENU_ALPHA 0.70f
#define OV_FLASH_NS 180000000ll   // a pressed button's flash fades to its held glow in this time
// front_android.c → gfx_vk_present.c: what to draw (0 nothing: a controller is attached, and its keys open the menu; 1 the gamepad; 2 only the menu
// button: the player hid the gamepad, or keys came from a device not counted as a controller), where a stick's knob is (its offset from the stick's centre
// in stick radii, -1..1), and when each control was pressed (0 = not held; for the d-pad, its four directions by DM_BTN_* bit; a stick: while a finger holds it)
int dm_overlay_visible(void);
void dm_overlay_knob(int stick, float *dx, float *dy);
int64_t dm_overlay_pressed(int ctl, uint64_t bit);   // CLOCK_MONOTONIC ns of the press, 0 = not held
// the player's own layout (the layout editor; the menu button can also be dragged any time, where it hides nothing of the game's): a control's centre as
// fractions of the window's width and height and its size factor, per layout, kept in settings.txt. 1 and x, y when the player moved the control, 0 when it
// is where the layout puts it; *s is its size factor either way (1 = the layout's)
int dm_overlay_custom(int layout, int ctl, float *x, float *y, float *s);
int dm_overlay_custom_ver(void);   // counts the changes (the theme-less drawing is rasterized again)
float dm_overlay_opacity(void);    // the gamepad's opacity, 0.2..0.8 (the menu's Opacity slider)
static inline int ov_index(const ov_ctl *c, int *layout) { int k = (int)(c - &OV_LAYOUTS[0][0]); *layout = k / OV_N; return k % OV_N; }   // c: in OV_LAYOUTS
static inline float ov_cscale(const ov_ctl *c) { int l, i = ov_index(c, &l); float x, y, s = 1; dm_overlay_custom(l, i, &x, &y, &s); return s; }
#define OV_DRAG_SLOP 0.02f   // a press on the menu button that moves this far (of the window's height) drags it instead of opening the menu
// the controls' size factor: 1, or more on a small screen so a face button stays at least OV_MIN_MM wide (sizes follow the window's height, which on a
// 6.7" phone made them 7 mm against 12 on an 8.8" tablet). Positions stay where they are
#define OV_MIN_MM 10.0f
float dm_overlay_scale(void);
// where a control is now, in window pixels: its centre x, y and half its width and height
static inline void ov_box(const ov_ctl *c, float W, float H, float b[4]) {   // (c: in OV_LAYOUTS, whose index finds the player's own place and size)
    int l, i = ov_index(c, &l); float s = dm_overlay_scale(), mx, my, cs = 1;
    if (dm_overlay_custom(l, i, &mx, &my, &cs)) { b[0] = mx * W; b[1] = my * H; }
    else { b[0] = c->ax * W + c->x * H; b[1] = c->y * H; }
    b[2] = c->w * H * s * cs / 2; b[3] = c->h * H * s * cs / 2;
}
// its corners' radius: a round control (as wide as high), the menu button (a pill) and the d-pad (its diagonals) are round; the shoulder buttons are rounded boxes
static inline float ov_corner(const ov_ctl *c, const float b[4]) { float m = fminf(b[2], b[3]); return c->w == c->h || c->kind == OV_MENU ? m : m * 0.35f; }
// the signed distance from (dx, dy), an offset from a rounded box's centre, to its edge (negative inside); hw, hh: half its width and height, rad: its corners
static inline float ov_box_sdf(float dx, float dy, float hw, float hh, float rad) {
    float qx = fabsf(dx) - hw + rad, qy = fabsf(dy) - hh + rad, px = qx > 0 ? qx : 0, py = qy > 0 ? qy : 0;
    return sqrtf(px * px + py * py) + fminf(fmaxf(qx, qy), 0) - rad;
}
// the same for a control (b: ov_box), (dx, dy) in pixels from its centre
static inline float ov_sdf(const ov_ctl *c, const float b[4], float dx, float dy) { return ov_box_sdf(dx, dy, b[2], b[3], ov_corner(c, b)); }
#define OV_REACH_MIN 0.045f   // a small button (-, +, the menu button) still takes touches this far from its centre (of the window's height)
// how far outside its edge a control still takes a touch, in pixels: a button a quarter of its size (at least to OV_REACH_MIN), a stick or the d-pad half
static inline float ov_reach(const ov_ctl *c, const float b[4], float H) {
    float m = fminf(b[2], b[3]);
    return c->kind == OV_BUTTON || c->kind == OV_MENU ? fmaxf(m * 0.25f, OV_REACH_MIN * H * dm_overlay_scale() - m) : m * 0.5f;
}
// a d-pad direction's circle in the theme-less drawing: centre offset and radius, in the d-pad's half width (k: 0 right, 1 left, 2 down, 3 up)
static inline void ov_dpad_dir(int k, float *ox, float *oy, float *r) { float s = 0.42f; *ox = k == 0 ? s * 1.2f : k == 1 ? -s * 1.2f : 0; *oy = k == 2 ? s * 1.2f : k == 3 ? -s * 1.2f : 0; *r = s * 0.75f; }
static const uint64_t OV_DPAD_BITS[4] = {DM_BTN_RIGHT, DM_BTN_LEFT, DM_BTN_DOWN, DM_BTN_UP};

// ---- the app's theme (overlay_theme.c): the gamepad drawn as its design's sprites, from the APK's assets/overlay (porter packs the game's pack from
// dm/android/themes). A sprite belongs to a control (and, for the d-pad, a direction), is drawn in one of its states, and sits at an offset from the control's
// centre; offsets and sizes are the design's CSS pixels, which the window's height scales (the design's height = the window's) ----
enum { OVT_IDLE, OVT_HELD, OVT_KNOB, OVT_DECOR };   // a button or a stick's base at rest or held; a stick's knob; a decoration drawn under the controls
typedef struct { int ctl, dir, state; float u, v, w, h, ox, oy; } ov_tsprite;   // dir: OV_DPAD_BITS's index, -1 none; u, v, w, h: in the atlas, texels; ox, oy: top left, CSS px
typedef struct { int layout, w, h; uint8_t *px; float dh, scale, travel, alpha; int n; ov_tsprite *sp; } ov_theme;   // px: w x h RGBA; dh: the design's height;
                                                                                                                       // scale: texels per CSS px; travel: the knob's, CSS px
const ov_theme *dm_overlay_theme(int layout);   // the theme's sprites for a layout, NULL when the app has none (it keeps its pixels until dm_overlay_theme_done)
void dm_overlay_theme_done(const ov_theme *t);   // the pixels are on the GPU: their memory can go

// ---- the pause menu: a panel in the middle of the window: a row of tabs, the open tab's rows, and Resume. Everything is placed in a panel rectangle p
// (x, y, width, height: the window's for touches, the raster's for drawing), so the touches and the drawing agree ----
enum { OVM_OVERLAY, OVM_OPACITY, OVM_EDIT, OVM_RUMBLE, OVM_FPS, OVM_MODE, OVM_RESUME, OVM_TAB, OVM_N };   // OVM_TAB: the tabs' row; OVM_EDIT: the layout editor;
                                                                                                        // OVM_MODE: handheld (performance) or docked (resolution)
#define OVM_TABS 2
#define OVM_PER_TAB 4
static const int OVM_TAB_ITEMS[OVM_TABS][OVM_PER_TAB] = {{OVM_OVERLAY, OVM_OPACITY, OVM_EDIT, OVM_RUMBLE}, {OVM_FPS, OVM_MODE, -1, -1}};
static const char *const OVM_TAB_NAMES[OVM_TABS] = {"Gamepad", "Game"};
#define OVM_ROWS (OVM_PER_TAB + 2)   // the tabs, a tab's rows (as many for every tab: the panel keeps its size), Resume
static inline int ov_menu_item(int tab, int row) { return row == 0 ? OVM_TAB : row == OVM_ROWS - 1 ? OVM_RESUME : row > 0 && row < OVM_ROWS - 1 ? OVM_TAB_ITEMS[tab & 1][row - 1] : -1; }
#define OV_MENU_ROW 0.115f   // row height, of the window's height
#define OV_MENU_W 0.95f      // panel width, of the window's height
#define OV_MENU_PAD 0.03f
#define OV_OPACITY_MIN 20    // the gamepad's opacity, percent (the Opacity slider, settings.txt opacity=)
#define OV_OPACITY_MAX 80
// what the menu shows (-1 = closed): bit 0 the gamepad is on, bit 1 rumble is on, bit 2 docked, bit 3 the frame rate shown, bits 4..7 the highlighted row + 1
// (0 = none), bit 8 the open tab, bits 9..15 the opacity in percent. Bit 16: the text entry's panel instead (its edit count below); bit 17: the layout editor
// (OVE_ST_* below)
int dm_menu_state(void);
static inline float ov_menu_scale(void) { float s = dm_overlay_scale(), fit = 0.92f / (OVM_ROWS * OV_MENU_ROW + 2 * OV_MENU_PAD); return s < fit ? s : fit; }
static inline void ov_menu_rect(float W, float H, float r[4]) {   // x, y, width, height in window pixels
    float s = ov_menu_scale(), pw = OV_MENU_W * H * s, ph = (OVM_ROWS * OV_MENU_ROW + 2 * OV_MENU_PAD) * H * s;
    if (pw > W * 0.96f) pw = W * 0.96f;
    r[0] = (W - pw) / 2; r[1] = (H - ph) / 2; r[2] = pw; r[3] = ph;
}
static inline void ov_menu_rowrect(const float p[4], int row, float r[4]) {   // row's rectangle in the panel p
    float u = p[3] / (OVM_ROWS * OV_MENU_ROW + 2 * OV_MENU_PAD);
    r[0] = p[0] + OV_MENU_PAD * u; r[1] = p[1] + (OV_MENU_PAD + row * OV_MENU_ROW) * u; r[2] = p[2] - 2 * OV_MENU_PAD * u; r[3] = OV_MENU_ROW * u;
}
static inline int ov_menu_row_at(float x, float y, float W, float H) {   // the row under (x, y): 0..OVM_ROWS-1, -1 the panel elsewhere, -2 outside it
    float r[4]; ov_menu_rect(W, H, r);
    if (x < r[0] || x >= r[0] + r[2] || y < r[1] || y >= r[1] + r[3]) return -2;
    float q[4]; ov_menu_rowrect(r, 0, q); int i = (int)floorf((y - q[1]) / q[3]);
    return i >= 0 && i < OVM_ROWS ? i : -1;
}
static inline int ov_menu_tab_at(float x, const float p[4]) { float r[4]; ov_menu_rowrect(p, 0, r); int t = (int)((x - r[0]) / (r[2] / OVM_TABS)); return t < 0 ? 0 : t >= OVM_TABS ? OVM_TABS - 1 : t; }
// the Opacity slider's track in its row (x, y of its left end's centre line, width): the right part of the row, room for the value after it
static inline void ov_menu_track(const float p[4], int row, float t[3]) { float r[4]; ov_menu_rowrect(p, row, r); t[0] = r[0] + r[2] * 0.40f; t[1] = r[1] + r[3] / 2; t[2] = r[2] * 0.44f; }
static inline int ov_menu_track_value(float x, const float p[4], int row) {   // the opacity a finger at x sets, whole 5 percents
    float t[3]; ov_menu_track(p, row, t); float f = (x - t[0]) / t[2]; f = f < 0 ? 0 : f > 1 ? 1 : f;
    return OV_OPACITY_MIN + 5 * (int)lroundf(f * (OV_OPACITY_MAX - OV_OPACITY_MIN) / 5);
}

// ---- the layout editor (front_android.c, from the menu's Edit layout): the game stays held; every control (the menu button too) can be dragged, and
// resized by a pinch or the toolbar's - and + (the tapped control, or all of them when none is). A toolbar under the top edge: its hint line and buttons;
// Cancel with changes asks first, in a prompt in the middle. Both are drawn into the menu panel's raster (gfx_vk_present.c ov_edit_panel) ----
enum { OVE_SMALLER, OVE_BIGGER, OVE_RESET, OVE_CANCEL, OVE_SAVE, OVE_N, OVE_DISCARD = OVE_N, OVE_KEEP, OVE_ALL };
// the editor's state in dm_menu_state: bit 17, bit 0 the prompt is open, bit 1 something changed, bits 4..7 the button held + 1, bits 8..15 the chosen control + 1
#define OVE_ST 0x20000
#define OVE_SCALE_MIN 0.5f   // a control's size factor
#define OVE_SCALE_MAX 2.0f
static inline void ov_ed_rect(float W, float H, float r[4]) {   // the toolbar, in window pixels: under the top edge, in the middle (below the menu button's place)
    float s = ov_menu_scale(); r[2] = fminf(0.85f * H * s, W * 0.96f); r[3] = 0.20f * H * s; r[0] = (W - r[2]) / 2; r[1] = 0.13f * H;
}
static inline void ov_ed_prompt_rect(float W, float H, float r[4]) {   // the discard prompt, in the middle
    float s = ov_menu_scale(); r[2] = fminf(0.75f * H * s, W * 0.9f); r[3] = 0.30f * H * s; r[0] = (W - r[2]) / 2; r[1] = (H - r[3]) / 2;
}
static inline void ov_ed_button(int i, const float p[4], float b[4]) {   // button i in its panel p (the toolbar's, or the prompt's for OVE_DISCARD, OVE_KEEP)
    static const float x0[OVE_ALL] = {0.15f, 1.30f, 2.80f, 5.25f, 7.70f, 0.50f, 5.20f}, w[OVE_ALL] = {1.0f, 1.0f, 2.30f, 2.30f, 2.15f, 4.30f, 4.30f};
    float u = p[2] / 10; b[0] = p[0] + x0[i] * u; b[2] = w[i] * u;
    if (i < OVE_N) { b[1] = p[1] + p[3] * 0.42f; b[3] = p[3] * 0.52f; } else { b[1] = p[1] + p[3] * 0.50f; b[3] = p[3] * 0.36f; }
}
static inline int ov_ed_button_at(float x, float y, const float p[4], int first, int n) {   // the button of first..first+n-1 under (x, y), -1 none
    for (int i = first; i < first + n; i++) { float b[4]; ov_ed_button(i, p, b); if (x >= b[0] && x < b[0] + b[2] && y >= b[1] && y < b[1] + b[3]) return i; }
    return -1;
}

// ---- the text entry's keyboard (front_android.c front_text_input, drawn by gfx_vk_present.c ov_text_panel), in the pause menu's panel rectangle ----
// Four rows of ten character keys and a row of four wide ones; p is the panel's rectangle (x, y, width, height: the window's for touches, the raster's for drawing).
// The panel: the title (its top 13%), the text field (to 28%), the keys (to 90%), the key hints
#define OVK_N 44
static const char *const OVK_ROWS[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-._"};
enum { OVK_SHIFT = 40, OVK_SPACE, OVK_DEL, OVK_OK };
static inline void ov_kb_key(int i, const float p[4], float r[4]) {   // key i's rectangle, a small gap inside its cell
    float pad = p[2] * 0.03f, w = p[2] - 2 * pad, top = p[1] + p[3] * 0.30f, kh = p[3] * 0.60f / 5;
    if (i < 40) { float kw = w / 10; r[0] = p[0] + pad + (i % 10) * kw; r[1] = top + (i / 10) * kh; r[2] = kw; r[3] = kh; }
    else { static const float x0[4] = {0, 0.2f, 0.65f, 0.8f}, ww[4] = {0.2f, 0.45f, 0.15f, 0.2f}; r[0] = p[0] + pad + x0[i - 40] * w; r[1] = top + 4 * kh; r[2] = ww[i - 40] * w; r[3] = kh; }
    float g = kh * 0.06f; r[0] += g; r[1] += g; r[2] -= 2 * g; r[3] -= 2 * g;
}
static inline int ov_kb_at(float x, float y, const float p[4]) {   // the key under (x, y), -1 none
    for (int i = 0; i < OVK_N; i++) { float r[4]; ov_kb_key(i, p, r); if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) return i; }
    return -1;
}
static inline int ov_kb_char(int i, int shift) {   // a character key's character (0 for the wide keys)
    if (i < 0 || i >= 40) return 0;
    int c = OVK_ROWS[i / 10][i % 10]; return shift && c >= 'a' && c <= 'z' ? c - 32 : c;
}
static inline int ov_kb_move(int i, int dx, int dy) {   // the d-pad's move from key i (rows wrap; the wide row maps to the columns above it)
    if (i < 0) return 0;
    static const int wide_col[4] = {0, 4, 7, 9};   // the column each wide key sits under
    int col = i < 40 ? i % 10 : wide_col[i - 40], row = i < 40 ? i / 10 : 4;
    if (dy) { row = (row + dy + 5) % 5; if (row == 4) return col < 2 ? OVK_SHIFT : col < 6 ? OVK_SPACE : col < 8 ? OVK_DEL : OVK_OK; return row * 10 + col; }
    if (row == 4) return 40 + (i - 40 + dx + 4) % 4;
    return row * 10 + (col + dx + 10) % 10;
}
