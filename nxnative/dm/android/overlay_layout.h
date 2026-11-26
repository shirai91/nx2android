// Android port: the on-screen gamepad's layout, shared by the touch handling (front_android.c) and its drawing (gfx_vk_present.c, with the app's theme: overlay_theme.c).
// Two layouts, the tablet's and the phone's (dm/android/overlay_default.tsv): a control's centre is x window heights from its anchor (the left edge, the middle
// or the right edge: a thumb reaches as far from the edge on any width) and y of the window's height down; its size is in window heights (round controls stay round)
#pragma once
#include <math.h>
#include <stdlib.h>
#include "dm_input.h"
// The overlay's UI language (implemented in front_android.c: DM_UI_LANG=zh|en, else device locale).
int dm_ui_zh(void);
#define TR(en, zh) (dm_ui_zh() ? (zh) : (en))
enum { OV_STICK_L, OV_STICK_R, OV_BUTTON, OV_DPAD, OV_MENU };   // OV_MENU: the menu button (shown with the gamepad on or off, 70%; env.txt DM_MENU_BUTTON=0 leaves it out): pauses the game and opens the settings
typedef struct { int kind; float ax, x, y, w, h; uint64_t btn; const char *label, *name; } ov_ctl;   // ax: the anchor, as a fraction of the window's width; name: the table's
// The layouts: dm/android/overlay_default.tsv with the game profile's moves (android.tsv overlay rows), compiled by build.py into android_profile.h. Both list the
// same controls in the same order, so a control's index means the same in either (a press keeps it across a change of layout)
#include "android_profile.h"
static const ov_ctl OV_LAYOUTS[2][DM_AOV_N] = {{DM_AOV_TABLET}, {DM_AOV_PHONE}};
#define OV_N DM_AOV_N
// Touch sticks start at the finger's down point; the right stick has finer low-speed aiming.
static inline void ov_touch_stick(float x, float y, float radius, int right, float *dx, float *dy) {
    if (radius <= 0) { *dx = *dy = 0; return; }
    float m = hypotf(x, y) / radius;
    if (m <= 0.05f) { *dx = *dy = 0; return; }
    float v = fminf((m - 0.05f) / 0.95f, 1.0f);
    if (right) v *= 0.7f + 0.3f * v;
    *dx = x / (m * radius) * v; *dy = y / (m * radius) * v;
}
// the layout in use: the phone's when the window is at least OV_PHONE_ASPECT wide for its height (a 20:9 phone is 2.2, a 16:10 tablet 1.6), else the tablet's
#define OV_PHONE_ASPECT 1.9f
// DM_OVERLAY_SPLIT=1 (start-up, off by default): a window narrower than 3:2 (an unfolded Galaxy Z Fold: 2184 x 1968; a 4:3 tablet; a split-screen window) can put the
// picture across its top, as wide as the window, and the gamepad in the band left under it, instead of over a picture letterboxed in the middle: the band is
// laid out as a window of its own (a 2184 x 740 band takes the phone's layout). gfx_vk_present.c places the picture to match while the gamepad shows, and the
// player's own positions (the layout editor, the menu button's drag) are kept against the band (ov_drag_place). A band under a quarter of the height is not used
#define OV_SPLIT_ASPECT 1.5f
// the band is used only where its face buttons can be at least this wide. Players found the unfolded Galaxy Z Fold7's 8.7 mm comfortable. A 4:3 tablet's
// 384-pixel band gives 6.2 mm and an older Z Fold 6.7 mm: those keep the gamepad over the picture. A window taller than wide takes the band whatever the size:
// there the sides of the gamepad over the picture meet in the middle (half of a Legion Y700 in split screen: the right stick over the d-pad), and its band gives 7.0 mm
#define OV_BAND_MIN_MM 8.0f
static inline int ov_split_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_OVERLAY_SPLIT"); on = e && atoi(e) > 0; } return on; }
int dm_external_active(void);   // (front_android.c) the picture is on an external display: the phone's window is all gamepad, with no picture to leave room for
int dm_overlay_dpi(void);   // the display's density, 0 = unknown
static inline float ov_band_face_mm(float W, float bandh, int dpi);
static inline int ov_band(float W, float H, float *y0, float *bh) {   // the band's top and height, for a W x H window; 0: no band
    if (!ov_split_on() || dm_external_active() || W <= 0 || H <= 0 || W / H >= OV_SPLIT_ASPECT) return 0;
    float ph = W * 9.0f / 16.0f, bandh = H - ph; if (bandh < 0.25f * H) return 0;   // (a 16:9 picture: every Switch game's)
    int dpi = dm_overlay_dpi(); if (W >= H && dpi > 0 && dpi < 0xfffe && ov_band_face_mm(W, bandh, dpi) < OV_BAND_MIN_MM) return 0;
    *y0 = ph; *bh = bandh; return 1;
}
static inline int ov_layout_for_size(int width, int height) {
    float y0, bh, h = (float)height; if (ov_band((float)width, h, &y0, &bh)) h = bh;
    return h > 0 && (float)width / h >= OV_PHONE_ASPECT; }
int dm_overlay_layout(void);   // front_android.c: 0 the tablet's, 1 the phone's
#define OV_CTLS (OV_LAYOUTS[dm_overlay_layout()])
#define OV_KNOB 0.42f        // a stick's knob radius, of the stick's (the theme-less drawing)
#define OV_MENU_ALPHA 0.70f
#define OV_FLASH_NS 180000000ll   // a pressed button's flash fades to its held glow in this time
// front_android.c → gfx_vk_present.c: what to draw (0 nothing; 1 the gamepad; 2 only the menu button). Show gamepad decides between 1 and 2 even with a
// controller attached. Where a stick's knob is (its offset from the stick's centre
// in stick radii, -1..1), and when each control was pressed (0 = not held; for the d-pad, its four directions by DM_BTN_* bit; a stick: while a finger holds it)
int dm_overlay_visible(void);
void dm_overlay_knob(int stick, float *dx, float *dy);
// Stick area (the pause menu's Gamepad tab, settings.txt stick_zone=; issue #589): 0 a stick takes the touches on and around its circle; 1 half screen: a touch
// that goes down on no control in the left half of the gamepad's window is the left stick's, in the right half the right stick's, centred where it went down
// (as a touch on the circle is: g_ptr_origin), so no touch of the game's touch screen is left there. dm_overlay_stick_at moves a held stick's centre (b: ov_box,
// on a W x H window) to that point; 0 when the stick is at its place
int dm_overlay_stick_zone(void);
int dm_overlay_stick_at(int stick, float W, float H, float b[4]);
int64_t dm_overlay_pressed(int ctl, uint64_t bit);   // CLOCK_MONOTONIC ns of the press, 0 = not held
// the player's own layout (the layout editor; the menu button can also be dragged any time, where it hides nothing of the game's): a control's centre as
// fractions of the window's width and height and its size factor, per layout, kept in settings.txt. 1 and x, y when the player moved the control, 0 when it
// is where the layout puts it; *s is its size factor either way (1 = the layout's)
int dm_overlay_custom(int layout, int ctl, float *x, float *y, float *s);
int dm_overlay_custom_ver(void);   // counts the changes (the theme-less drawing is rasterized again)
// a control the player hid in the layout editor (Hide): not drawn and not touchable in play. In the editor it is drawn and can be chosen, so it can be
// shown again: dm_overlay_hidden says whether it is hidden, dm_overlay_off whether play leaves it out now (hidden and the editor closed). The player never hides the menu button; DM_MENU_BUTTON=0 does (front_android.c g_menu_btn)
int dm_overlay_hidden(int layout, int ctl);
int dm_overlay_off(int layout, int ctl);
static inline int ov_off(const ov_ctl *c);   // (below: c in OV_LAYOUTS)
float dm_overlay_opacity(void);    // the gamepad's opacity, 0.2..0.8 (the menu's Opacity slider)
static inline int ov_index(const ov_ctl *c, int *layout) { int k = (int)(c - &OV_LAYOUTS[0][0]); *layout = k / OV_N; return k % OV_N; }   // c: in OV_LAYOUTS
static inline float ov_cscale(const ov_ctl *c) { int l, i = ov_index(c, &l); float x, y, s = 1; dm_overlay_custom(l, i, &x, &y, &s); return s; }
static inline int ov_off(const ov_ctl *c) { int l, i = ov_index(c, &l); return dm_overlay_off(l, i); }
#define OV_DRAG_SLOP 0.02f   // a press on the menu button that moves this far (of the window's height) drags it instead of opening the menu
// the controls' size factor: 1, or more on a small screen so a face button stays at least OV_MIN_MM wide (sizes follow the window's height, which on a
// 6.7" phone made them 7 mm against 12 on an 8.8" tablet). A side's controls grow out from its bottom corner with their size (ov_box), so they keep their
// spacing; the factor stops where they would leave the window (ov_fit_scale). Grown around their own centres, the Odin 3's x1.45 made eleven pairs overlap
#define OV_MIN_MM 10.0f
float dm_overlay_scale(void);
// the top strip of layout L at scale 1: down to the menu button's bottom edge (the middle's controls), where the frame rate counter's FPS row also sits
// (gfx_vk_present.c ov_fps: to about 0.06 of the height). A side control whose top edge is above it sits beside the menu button (the phone's - and +), the
// others below it. The counter's temperature rows are not part of the strip: with them the counter reaches 0.09 (one row) or 0.12 (both) of the height,
// and on the tablet layout a grown - starts under them (the Odin 3 at x1.20: - from 0.079, so the rows cover its top; the counter takes no touches).
// Keeping all three rows clear would hold the Odin 3 at about x1.12, smaller controls for every player for a diagnostic few turn on
static inline float ov_strip(const ov_ctl *L) { float top = 0; for (int i = 0; i < OV_N; i++) if (L[i].ax == 0.5f) top = fmaxf(top, L[i].y + L[i].h / 2); return top; }
static inline int ov_beside(const ov_ctl *L, const ov_ctl *c) { return c->ax != 0.5f && c->y - c->h / 2 < ov_strip(L); }
// where a control is now, in window pixels: its centre x, y and half its width and height. A side's control (anchored to the left or the right edge) is
// x window heights from its edge and 1 - y up from the bottom at scale 1; at scale s both distances are s times as far, so the whole side scales about
// its bottom corner. The menu button (anchored to the middle) stays at the top middle, growing down from the top edge, and so do the controls beside it
// (ov_beside: they scale about their side's top corner, y down from the top edge)
static inline void ov_box(const ov_ctl *c, float W, float H, float b[4]) {   // (c: in OV_LAYOUTS, whose index finds the player's own place and size)
    float y0 = 0, bh; if (ov_band(W, H, &y0, &bh)) H = bh;   // (ov_band: the band under the picture is the gamepad's window)
    int l, i = ov_index(c, &l); float s = dm_overlay_scale(), mx, my, cs = 1;
    if (dm_overlay_custom(l, i, &mx, &my, &cs)) { b[0] = mx * W; b[1] = my * H; }
    else if (c->ax == 0.5f) { b[0] = c->ax * W + c->x * H; b[1] = c->y * H * s; }
    else if (ov_beside(OV_LAYOUTS[l], c)) { b[0] = c->ax * W + c->x * H * s; b[1] = c->y * H * s; }
    else { b[0] = c->ax * W + c->x * H * s; b[1] = H - (1 - c->y) * H * s; }
    b[2] = c->w * H * s * cs / 2; b[3] = c->h * H * s * cs / 2; b[1] += y0;
}
// the largest scale up to s (and at least 1) at which the side controls of layout L, grown as ov_box places them, stay on a W x H window and apart:
// - a control below the top strip stays inside its half of the width (the two sides never meet) and below the strip, which reaches down to the menu
//   button's bottom edge and the bottom edges of the controls beside it, grown as they grow (on the Odin 3, x1.30 put - under the counter's FPS row in the top left)
// - a control beside the strip (the phone's - and +) stays clear of the menu button and on the screen; growing out from its side's top corner, it only
//   moves away from that corner, so the frame rate counter there that it clears at x1 it clears at any scale
static inline float ov_fit_scale(const ov_ctl *L, float W, float H, float s) {
    if (W <= 0 || H <= 0) return s;
    float top = ov_strip(L), strip = top, mid = 0;
    for (int i = 0; i < OV_N; i++) {
        const ov_ctl *c = &L[i];
        if (c->ax == 0.5f) mid = fmaxf(mid, fabsf(c->x) + c->w / 2);
        else if (c->y - c->h / 2 < top) strip = fmaxf(strip, c->y + c->h / 2);
    }
    for (int i = 0; i < OV_N; i++) {
        const ov_ctl *c = &L[i]; if (c->ax == 0.5f) continue;
        float out = fabsf(c->x) + c->w / 2;
        if (c->y - c->h / 2 < top) {
            if (c->y + c->h / 2 > 0) s = fminf(s, 1 / (c->y + c->h / 2));
            if (out + mid > 0) s = fminf(s, W / H / 2 / (out + mid));
        } else {
            float up = 1 - c->y + c->h / 2;
            if (up + strip > 0) s = fminf(s, 1 / (up + strip));
            if (out > 0) s = fminf(s, W / H / 2 / out);
        }
    }
    return fmaxf(s, 1);
}
static inline float ov_band_face_mm(float W, float bandh, int dpi) {
    float face = 0.1f; const ov_ctl *L = OV_LAYOUTS[W / bandh >= OV_PHONE_ASPECT]; for (int i = 0; i < OV_N; i++) if (L[i].kind == OV_BUTTON && L[i].btn == DM_BTN_A) face = L[i].w;
    float mm = face * bandh / (float)dpi * 25.4f, s = OV_MIN_MM / mm; if (s < 1) s = 1; if (s > 1.45f) s = 1.45f;
    return mm * ov_fit_scale(L, W, bandh, s);
}
// a dragged control's centre on one axis (half: its half size, extent: the window's): kept on the screen, and with step > 0 on the nearest multiple
// of step that keeps it there (none may fit an oversized control)
static inline float ov_drag_pos(float p, float half, float extent, float step) {
    float lo = fminf(half, extent / 2), hi = extent - lo;
    if (step > 0) {
        float first = ceilf(lo / step), last = floorf(hi / step);
        if (first <= last) return fminf(fmaxf(roundf(p / step), first), last) * step;
    }
    return fminf(fmaxf(p, lo), hi);
}
// where a control dragged to (x, y) on a W x H window is kept: its centre as fractions of the window's width and of the gamepad's height, as ov_box reads
// them back (with a band under the picture, of the band and from its top, and kept inside it). steps: the grid rows over that height (0: none)
static inline void ov_drag_place(float x, float y, float hw, float hh, float W, float H, int steps, float *fx, float *fy) {
    float y0 = 0, bh = H; ov_band(W, H, &y0, &bh);
    float step = steps > 0 ? bh / (float)steps : 0;
    *fx = ov_drag_pos(x, hw, W, step) / W; *fy = ov_drag_pos(y - y0, hh, bh, step) / bh;
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
// A small area between adjacent face buttons lets one thumb press both without changing their individual hit areas.
static inline uint64_t ov_face_combo(const ov_ctl *layout, float x, float y, float W, float H) {
    const uint64_t face = DM_BTN_A | DM_BTN_B | DM_BTN_X | DM_BTN_Y;
    for (int i = 0; i < OV_N; i++) {
        const ov_ctl *a = &layout[i]; if (a->kind != OV_BUTTON || !(a->btn & face) || ov_off(a)) continue;
        for (int j = i + 1; j < OV_N; j++) {
            const ov_ctl *b = &layout[j]; uint64_t pair = a->btn | b->btn;
            if (b->kind != OV_BUTTON || !(b->btn & face) || ov_off(b) || a->btn == b->btn || pair == (DM_BTN_A | DM_BTN_Y) || pair == (DM_BTN_X | DM_BTN_B)) continue;
            float ab[4], bb[4]; ov_box(a, W, H, ab); ov_box(b, W, H, bb);
            float distance = hypotf(ab[0] - bb[0], ab[1] - bb[1]);
            float radius = fminf(fminf(ab[2], ab[3]), fminf(bb[2], bb[3]));
            if (distance < radius || distance > radius * 3.5f) continue;
            if (hypotf(x - (ab[0] + bb[0]) * 0.5f, y - (ab[1] + bb[1]) * 0.5f) <= radius * 0.35f) return pair;
        }
    }
    return 0;
}
static inline uint64_t ov_button_bits(const ov_ctl *layout, const ov_ctl *button, float x, float y, float W, float H) {
    uint64_t pair = (button->btn & (DM_BTN_A | DM_BTN_B | DM_BTN_X | DM_BTN_Y)) ? ov_face_combo(layout, x, y, W, H) : 0;
    return pair && (pair & button->btn) ? pair : button->btn;
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

// ---- the frame rate counter's options (settings.txt fps_rows, fps_pos, fps_colour, fps_outline, fps_size), packed in one int for the backend (dm_fps_opts) ----
enum { FPSO_FPS = 1, FPSO_SOC = 2, FPSO_BAT = 4, FPSO_DRV = 8, FPSO_GRAPH = 16, FPSO_ALL = 31, FPSO_ROWS_DEFAULT = FPSO_FPS | FPSO_SOC | FPSO_BAT };   // fps_rows: the rows shown, a bit each (the driver row: the Vulkan driver in use; the graph: the frame times, one bar a frame); by default FPS, SOC and BAT, the counter before these options (the driver row and the graph would make it reach the - button or L and ZL on a 1920x1080 window)
enum { FPSO_TL, FPSO_TR, FPSO_BL, FPSO_BR, FPSO_POS_N };   // fps_pos: the corner (top left, top right, bottom left, bottom right)
enum { FPSO_WHITE, FPSO_YELLOW, FPSO_GREEN, FPSO_CYAN, FPSO_MAGENTA, FPSO_COLOUR_N };   // fps_colour: the text (a temperature too hot stays red)
enum { FPSO_BOX, FPSO_NONE, FPSO_THIN, FPSO_MEDIUM, FPSO_THICK, FPSO_OUTLINE_N };   // fps_outline: the dark block behind the rows; nothing; a black outline round each letter, 1, 2 or 3 font texels
enum { FPSO_SMALL, FPSO_NORMAL, FPSO_LARGE, FPSO_XL, FPSO_SIZE_N };   // fps_size: the letters' scale = base - 1, base, base + 1, 2 * base (base: 2.2% of the window's height / PF_CAP, at least 2)
static const char *const FPSO_POS_NAMES_EN[FPSO_POS_N] = {"Top left", "Top right", "Bottom left", "Bottom right"};
static const char *const FPSO_POS_NAMES_ZH[FPSO_POS_N] = {"左上", "右上", "左下", "右下"};
#define FPSO_POS_NAMES (dm_ui_zh() ? FPSO_POS_NAMES_ZH : FPSO_POS_NAMES_EN)
static const char *const FPSO_COLOUR_NAMES_EN[FPSO_COLOUR_N] = {"White", "Yellow", "Green", "Cyan", "Magenta"};
static const char *const FPSO_COLOUR_NAMES_ZH[FPSO_COLOUR_N] = {"白色", "黄色", "绿色", "青色", "洋红"};
#define FPSO_COLOUR_NAMES (dm_ui_zh() ? FPSO_COLOUR_NAMES_ZH : FPSO_COLOUR_NAMES_EN)
static const char *const FPSO_OUTLINE_NAMES_EN[FPSO_OUTLINE_N] = {"Box", "None", "Thin", "Medium", "Thick"};
static const char *const FPSO_OUTLINE_NAMES_ZH[FPSO_OUTLINE_N] = {"黑底", "无", "细边", "中边", "粗边"};
#define FPSO_OUTLINE_NAMES (dm_ui_zh() ? FPSO_OUTLINE_NAMES_ZH : FPSO_OUTLINE_NAMES_EN)
static const char *const FPSO_SIZE_NAMES_EN[FPSO_SIZE_N] = {"Small", "Normal", "Large", "Extra large"};
static const char *const FPSO_SIZE_NAMES_ZH[FPSO_SIZE_N] = {"小", "标准", "大", "特大"};
#define FPSO_SIZE_NAMES (dm_ui_zh() ? FPSO_SIZE_NAMES_ZH : FPSO_SIZE_NAMES_EN)
static const uint8_t FPSO_RGB[FPSO_COLOUR_N][3] = {{255, 255, 255}, {255, 220, 0}, {64, 255, 96}, {0, 230, 255}, {255, 64, 255}};
#define FPSO_DEFAULT (FPSO_ROWS_DEFAULT | FPSO_TL << 5 | FPSO_WHITE << 7 | FPSO_BOX << 10 | FPSO_NORMAL << 13)
static inline int fpso_rows(int o) { return o & 31; }
static inline int fpso_pos(int o) { return o >> 5 & 3; }
static inline int fpso_colour(int o) { return o >> 7 & 7; }
static inline int fpso_outline(int o) { return o >> 10 & 7; }
static inline int fpso_size(int o) { return o >> 13 & 3; }
static inline int fpso_pack(int rows, int pos, int colour, int outline, int size) { return rows | pos << 5 | colour << 7 | outline << 10 | size << 13; }
static inline int fpso_scale(int base, int size) { return size == FPSO_SMALL ? (base > 1 ? base - 1 : 1) : size == FPSO_LARGE ? base + 1 : size == FPSO_XL ? 2 * base : base; }
// ---- the pause menu: a panel in the middle of the window: a row of tabs, the open tab's rows, and Resume. Everything is placed in a panel rectangle p
// (x, y, width, height: the window's for touches, the raster's for drawing), so the touches and the drawing agree ----
enum { OVM_OVERLAY, OVM_OPACITY, OVM_EDIT, OVM_RUMBLE, OVM_FPS, OVM_SR, OVM_SHARP, OVM_MODE, OVM_EXPORT, OVM_IMPORT, OVM_PAD, OVM_AUDIO,
       OVM_ROW_FPS, OVM_ROW_SOC, OVM_ROW_BAT, OVM_ROW_DRV, OVM_ROW_GRAPH, OVM_POS, OVM_COLOUR, OVM_OUTLINE, OVM_SIZE, OVM_ZONE, OVM_RESUME, OVM_TAB, OVM_N };   // OVM_TAB: the tabs' row;
            // OVM_ZONE: the sticks' touch area, their circles or half the screen each (dm_overlay_stick_zone);
            // OVM_ROW_*: the counter's rows on or off; OVM_POS .. OVM_SIZE: its corner, colour, outline and size, one value on with each press (fpso_step below);
            // OVM_EDIT: the layout editor; OVM_MODE: handheld (performance) or docked (resolution); OVM_EXPORT, OVM_IMPORT: the save manager (saves_android.c);
            // OVM_SR: Super resolution (the present pass upscales the game's picture into a larger window); OVM_SHARP: its Sharpness slider (0 to 100 percent by 25);
            // OVM_PAD: a controller's face buttons by their position or by their label (front_android.c pad_labels)
#define OVM_TABS 4   // (a power of two: ov_menu_item masks the tab)
#define OVM_PER_TAB 7   // (a tab's rows: as many for every tab, the panel keeps its size; Audio output sits with the gamepad's settings)
static const int OVM_TAB_ITEMS[OVM_TABS][OVM_PER_TAB] = {{OVM_OVERLAY, OVM_OPACITY, OVM_EDIT, OVM_ZONE, OVM_RUMBLE, OVM_PAD, OVM_AUDIO},
                                                         {OVM_FPS, OVM_SR, OVM_SHARP, OVM_MODE, OVM_EXPORT, OVM_IMPORT, -1},
                                                         {OVM_ROW_FPS, OVM_ROW_SOC, OVM_ROW_BAT, OVM_ROW_DRV, OVM_ROW_GRAPH, -1, -1},
                                                         {OVM_POS, OVM_COLOUR, OVM_OUTLINE, OVM_SIZE, -1, -1, -1}};
static const char *const OVM_TAB_NAMES_EN[OVM_TABS] = {"Gamepad", "Game", "Counter", "Style"};
static const char *const OVM_TAB_NAMES_ZH[OVM_TABS] = {"手柄", "游戏", "计数", "样式"};
#define OVM_TAB_NAMES (dm_ui_zh() ? OVM_TAB_NAMES_ZH : OVM_TAB_NAMES_EN)
#define OVM_ROWS (OVM_PER_TAB + 2)   // logical row capacity: tabs, body actions, Resume
#define OVM_VISIBLE 5              // fixed-size body viewport; extra items scroll instead of shrinking all text
#define OVM_VIEW_ROWS (OVM_VISIBLE + 2)
#define OVM_SCROLL_UNITS 1024      // fixed-point rows for continuous dragging, with no row snapping
int dm_menu_scroll_position(void);
static inline float ov_menu_scroll_rows(int position) { return (float)position / OVM_SCROLL_UNITS; }
static inline uint64_t ov_menu_raster_key(int state, int position) { return state < 0 ? UINT64_MAX : (uint64_t)(uint32_t)state << 32 | (uint32_t)position; }
static inline int fpso_step(int o, int item) {   // the options after a press on a menu item: a row's bit flips, the others go to their next value (and round)
    int r = fpso_rows(o), p = fpso_pos(o), c = fpso_colour(o), l = fpso_outline(o), z = fpso_size(o);
    switch (item) {
    case OVM_ROW_FPS: r ^= FPSO_FPS; break;
    case OVM_ROW_SOC: r ^= FPSO_SOC; break;
    case OVM_ROW_BAT: r ^= FPSO_BAT; break;
    case OVM_ROW_DRV: r ^= FPSO_DRV; break;
    case OVM_ROW_GRAPH: r ^= FPSO_GRAPH; break;
    case OVM_POS: p = (p + 1) % FPSO_POS_N; break;
    case OVM_COLOUR: c = (c + 1) % FPSO_COLOUR_N; break;
    case OVM_OUTLINE: l = (l + 1) % FPSO_OUTLINE_N; break;
    case OVM_SIZE: z = (z + 1) % FPSO_SIZE_N; break;
    default: break;
    }
    return fpso_pack(r, p, c, l, z);
}
int dm_menu_audio_available(void);
static inline int ov_menu_item(int tab, int row) {
    int item = row == 0 ? OVM_TAB : row == OVM_ROWS - 1 ? OVM_RESUME : row > 0 && row < OVM_ROWS - 1 ? OVM_TAB_ITEMS[tab & (OVM_TABS - 1)][row - 1] : -1;
    return item == OVM_AUDIO && !dm_menu_audio_available() ? -1 : item;
}
#define OV_MENU_ROW 0.115f   // row height, of the window's height
#define OV_MENU_W 0.95f      // panel width, of the window's height
#define OV_MENU_PAD 0.03f
#define OV_OPACITY_MIN 20    // the gamepad's opacity, percent (the Opacity slider, settings.txt opacity=)
#define OV_OPACITY_MAX 80
// what the menu shows (-1 = closed): bit 0 the gamepad is on, bit 1 rumble is on, bit 2 docked, bit 3 the frame rate shown, bits 4..7 the highlighted row + 1
// (0 = none), bit 8 the open tab's first bit and bit 19 its second (the Counter and Style tabs), bits 9..12 the opacity as steps of 5 from OV_OPACITY_MIN,
// bits 13..15 the sharpness in 25 percent steps, bit 23 the super resolution is on, bits 20..22 the last export's outcome (OVX_*), bits 24..27 a count of
// exports (its name changes with each), bit 28 the face buttons by label, bits 29..30 a count of the counter options' changes (dm_fps_opts: the panel shows
// them); bit 31 is never set (a negative state reads as "no menu", which is how a closed one is named). Bit 16: the text entry's panel instead (its edit
// count below); bit 17: the layout editor (OVE_ST_* below); bit 18: the save picker (OVS_ST)
#define OVT_ST 0x10000   // the mode bits of dm_menu_state, one each: OVT_ST the text entry, OVE_ST the layout editor, OVS_ST the save picker (tests/test_menu_bits.py)
int dm_menu_state(void);
enum { OVX_IDLE, OVX_BUSY, OVX_DONE, OVX_NOSAVE, OVX_FAILED };   // Export save: the button; copying; the export's name; the game has not saved yet; failed
int dm_save_note(char *out, int cap);   // front_android.c: the outcome (OVX_*) and the last export's name, or why it failed
static inline float ov_menu_scale(void) { float s = dm_overlay_scale(), fit = 0.92f / (OVM_VIEW_ROWS * OV_MENU_ROW + 2 * OV_MENU_PAD); return s < fit ? s : fit; }
static inline void ov_menu_rect(float W, float H, float r[4]) {   // x, y, width, height in window pixels
    float s = ov_menu_scale(), pw = OV_MENU_W * H * s, ph = (OVM_VIEW_ROWS * OV_MENU_ROW + 2 * OV_MENU_PAD) * H * s;
    if (pw > W * 0.96f) pw = W * 0.96f;
    r[0] = (W - pw) / 2; r[1] = (H - ph) / 2; r[2] = pw; r[3] = ph;
}
static inline void ov_menu_rowrect(const float p[4], float slot, float r[4]) {
    float u = p[3] / (OVM_VIEW_ROWS * OV_MENU_ROW + 2 * OV_MENU_PAD);
    r[0] = p[0] + OV_MENU_PAD * u; r[1] = p[1] + (OV_MENU_PAD + slot * OV_MENU_ROW) * u; r[2] = p[2] - 2 * OV_MENU_PAD * u; r[3] = OV_MENU_ROW * u;
}
static inline int ov_menu_row_at(float x, float y, float W, float H) {   // the row under (x, y): 0..OVM_VIEW_ROWS-1, -1 the panel elsewhere, -2 outside it
    float r[4]; ov_menu_rect(W, H, r);
    if (x < r[0] || x >= r[0] + r[2] || y < r[1] || y >= r[1] + r[3]) return -2;
    float q[4]; ov_menu_rowrect(r, 0, q); int i = (int)floorf((y - q[1]) / q[3]);
    return i >= 0 && i < OVM_VIEW_ROWS ? i : -1;
}
static inline int ov_menu_body_count(int tab) {
    int n = 0; for (int row = 1; row < OVM_ROWS - 1; row++) n += ov_menu_item(tab, row) >= 0; return n;
}
static inline int ov_menu_scroll_limit(int tab) { int n = ov_menu_body_count(tab) - OVM_VISIBLE; return n > 0 ? n : 0; }
static inline int ov_menu_body_row(int tab, int index) {
    int n = 0; for (int row = 1; row < OVM_ROWS - 1; row++) if (ov_menu_item(tab, row) >= 0 && n++ == index) return row;
    return -1;
}
static inline int ov_menu_body_index(int tab, int row) {
    int n = 0; for (int r = 1; r < OVM_ROWS - 1; r++) if (ov_menu_item(tab, r) >= 0) { if (r == row) return n; n++; }
    return -1;
}
static inline float ov_menu_scroll_slot(int tab, float offset, int row) {
    return row == 0 ? 0 : row == OVM_ROWS - 1 ? OVM_VIEW_ROWS - 1 : 1 + ov_menu_body_index(tab, row) - offset;
}
static inline int ov_menu_scroll_rowrect(const float p[4], int tab, float offset, int row, float r[4]) {
    if (ov_menu_item(tab, row) < 0) return 0;
    float slot = ov_menu_scroll_slot(tab, offset, row); ov_menu_rowrect(p, slot, r);
    return row == 0 || row == OVM_ROWS - 1 || (slot + 1 > 1 && slot < OVM_VIEW_ROWS - 1);
}
static inline int ov_menu_scroll_row_at(float x, float y, float W, float H, int tab, float offset) {
    float p[4], r[4]; ov_menu_rect(W, H, p);
    if (x < p[0] || x >= p[0] + p[2] || y < p[1] || y >= p[1] + p[3]) return -2;
    ov_menu_rowrect(p, 0, r); float slot = (y - r[1]) / r[3];
    if (slot < 0 || slot >= OVM_VIEW_ROWS) return -1;
    if (slot < 1) return 0;
    if (slot >= OVM_VIEW_ROWS - 1) return OVM_ROWS - 1;
    return ov_menu_body_row(tab, (int)floorf(slot - 1 + offset));
}
static inline float ov_menu_scroll_reveal(int tab, float offset, int row) {
    int n = 0; for (int r = 1; r < OVM_ROWS - 1; r++) if (ov_menu_item(tab, r) >= 0) {
        if (r == row) { if (n < offset) offset = n; else if (n + 1 > offset + OVM_VISIBLE) offset = n - OVM_VISIBLE + 1; break; } n++;
    }
    int limit = ov_menu_scroll_limit(tab); return offset < 0 ? 0 : offset > limit ? limit : offset;
}
static inline int ov_menu_scroll_drag(int tab, int start, float delta_rows) {
    int value = start + (int)lroundf(delta_rows * OVM_SCROLL_UNITS), limit = ov_menu_scroll_limit(tab) * OVM_SCROLL_UNITS;
    return value < 0 ? 0 : value > limit ? limit : value;
}
static inline int ov_menu_tab_at(float x, const float p[4]) { float r[4]; ov_menu_rowrect(p, 0, r); int t = (int)((x - r[0]) / (r[2] / OVM_TABS)); return t < 0 ? 0 : t >= OVM_TABS ? OVM_TABS - 1 : t; }
// the Opacity slider's track in its row (x, y of its left end's centre line, width): the right part of the row, room for the value after it
static inline void ov_menu_track(const float p[4], float row, float t[3]) { float r[4]; ov_menu_rowrect(p, row, r); t[0] = r[0] + r[2] * 0.40f; t[1] = r[1] + r[3] / 2; t[2] = r[2] * 0.44f; }
static inline int ov_menu_track_value(float x, const float p[4], float row) {   // the opacity a finger at x sets, whole 5 percents
    float t[3]; ov_menu_track(p, row, t); float f = (x - t[0]) / t[2]; f = f < 0 ? 0 : f > 1 ? 1 : f;
    return OV_OPACITY_MIN + 5 * (int)lroundf(f * (OV_OPACITY_MAX - OV_OPACITY_MIN) / 5);
}
static inline int ov_menu_track_pct(float x, const float p[4], float row) {   // the same track as a percent of its width 0..100 (the Sharpness slider)
    float t[3]; ov_menu_track(p, row, t); float f = (x - t[0]) / t[2]; f = f < 0 ? 0 : f > 1 ? 1 : f;
    return (int)lroundf(f * 100.0f);
}

// ---- the layout editor (front_android.c, from the menu's Edit layout): the game stays held; every control (the menu button too) can be dragged, and
// resized by a pinch or the toolbar's - and + (the tapped control, or all of them when none is). A toolbar under the top edge: its hint line and buttons;
// Cancel with changes asks first, in a prompt in the middle. Both are drawn into the menu panel's raster (gfx_vk_present.c ov_edit_panel) ----
enum { OVE_SMALLER, OVE_BIGGER, OVE_RESET, OVE_CANCEL, OVE_SAVE, OVE_GRID, OVE_HIDE, OVE_N, OVE_DISCARD = OVE_N, OVE_KEEP, OVE_ALL };   // OVE_HIDE: hide / show the chosen control
// the editor's state in dm_menu_state: bit 17, bit 0 the prompt is open, bit 1 something changed, bit 2 grid snapping, bit 3 the chosen control is hidden,
// bits 4..7 the button held + 1, bits 8..15 the chosen control + 1
#define OVE_ST 0x20000
#define OVE_GRID_ON 4
#define OVE_SEL_HIDDEN 8
#define OVE_GRID_ROWS 20   // Snap to grid's rows; its cells are square
#define OVE_SCALE_MIN 0.5f   // a control's size factor
#define OVE_SCALE_MAX 2.0f
static inline void ov_ed_rect(float W, float H, float r[4]) {   // the toolbar, in window pixels: under the top edge, in the middle (below the menu button's place)
    float s = ov_menu_scale(); r[2] = fminf(0.85f * H * s, W * 0.96f); r[3] = 0.30f * H * s; r[0] = (W - r[2]) / 2; r[1] = 0.13f * H;
}
static inline void ov_ed_prompt_rect(float W, float H, float r[4]) {   // the discard prompt, in the middle
    float s = ov_menu_scale(); r[2] = fminf(0.75f * H * s, W * 0.9f); r[3] = 0.30f * H * s; r[0] = (W - r[2]) / 2; r[1] = (H - r[3]) / 2;
}
static inline void ov_ed_button(int i, const float p[4], float b[4]) {   // button i in its panel p (the toolbar's, or the prompt's for OVE_DISCARD, OVE_KEEP)
    static const float x0[OVE_ALL] = {0.15f, 1.30f, 2.80f, 5.25f, 7.70f, 0.15f, 7.70f, 0.50f, 5.20f}, w[OVE_ALL] = {1.0f, 1.0f, 2.30f, 2.30f, 2.15f, 7.40f, 2.15f, 4.30f, 4.30f};
    static const float y0[OVE_ALL] = {0.28f, 0.28f, 0.28f, 0.28f, 0.28f, 0.68f, 0.68f, 0.50f, 0.50f}, h[OVE_ALL] = {0.34f, 0.34f, 0.34f, 0.34f, 0.34f, 0.28f, 0.28f, 0.36f, 0.36f};
    float u = p[2] / 10; b[0] = p[0] + x0[i] * u; b[1] = p[1] + y0[i] * p[3]; b[2] = w[i] * u; b[3] = h[i] * p[3];
}
static inline int ov_ed_button_at(float x, float y, const float p[4], int first, int n) {   // the button of first..first+n-1 under (x, y), -1 none
    for (int i = first; i < first + n; i++) { float b[4]; ov_ed_button(i, p, b); if (x >= b[0] && x < b[0] + b[2] && y >= b[1] && y < b[1] + b[3]) return i; }
    return -1;
}

// ---- the save picker (front_android.c, from the menu's Import save): the pause menu's panel and rows, with what front_android.c puts in each row: the saves in
// saves/ newest first, OVM_VIEW_ROWS - 2 to a page (left and right page), then the confirmation, then the note that the game closes. dm_menu_state: bit 18, bits 4..7
// the highlighted row + 1, bits 20..27 the picker's edit count (a page, a confirmation) ----
#define OVS_ST 0x40000
enum { OVS_EMPTY, OVS_TITLE, OVS_ITEM, OVS_NOTE, OVS_BUTTON, OVS_BACK };   // a row: nothing; the title (left) and the page (right); a save (its name, its size);
typedef struct { int style; char left[80], right[40]; } ov_srow;            // a line of text; a button (the import); Back or Cancel, centred as Resume is
int dm_save_rows(ov_srow rows[OVM_VIEW_ROWS]);   // front_android.c: the picker's rows, 0 when it is not open

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
