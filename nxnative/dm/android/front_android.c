// Android port: the host front end (dm/host/front.h), in two forms.
//   dmhost executable (adb shell): headless, the guest runs on the calling thread as on the Mac without DM_WINDOW; no window, no input, no text entry.
//   libdmhost.so in the app (a NativeActivity, no Java): ANativeActivity_onCreate lays out the game's files in the app's external files folder
//   (/sdcard/Android/data/<package>/files: mods/ = the rewritten modules, RomFS/, shaders.dmsv; env.txt = KEY=VALUE switches; log.txt = the log), keeps saves in
//   internal storage, and starts the host on a thread of its own. The window goes to the Vulkan backend's swapchain (dm_front_window); game controllers
//   (KEYCODE_BUTTON_*, sticks, triggers, hat) feed player 1's pad; touches go to the game's touch screen; shaking the device shakes player 1's controller;
//   leaving and coming back pauses as on iOS.
#include "dm.h"
#include "front.h"
#include "dm_input.h"
#include "overlay_layout.h"
#include <math.h>
#include <time.h>
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/window.h>
#include <android/native_window.h>
#include <android/sensor.h>
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void front_run(void (*guest)(void)) { guest(); }
void *front_metal_layer(void) { return NULL; }
int front_screen_max_fps(void) { return 0; }
void front_screen_match(int fps) { (void)fps; }
// front_text_input (the game's software keyboard, hle_swkbd.c) is further down, with the text entry

// ---------------- the app ----------------
static ANativeActivity *g_act;
AAssetManager *dm_front_assets(void) { return g_act ? g_act->assetManager : NULL; }   // the APK's assets (overlay_theme.c: the gamepad's theme)
static pthread_mutex_t g_win_mu = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *g_win;
ANativeWindow *dm_front_window(void) { pthread_mutex_lock(&g_win_mu); ANativeWindow *w = g_win; pthread_mutex_unlock(&g_win_mu); return w; }
__attribute__((weak)) void vk_window_lost(void) {}   // the backend lets go of the window's swapchain before the window goes
static void on_window_created(ANativeActivity *a, ANativeWindow *w) { (void)a; pthread_mutex_lock(&g_win_mu); g_win = w; pthread_mutex_unlock(&g_win_mu); }
static void on_window_destroyed(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; pthread_mutex_lock(&g_win_mu); g_win = NULL; pthread_mutex_unlock(&g_win_mu); vk_window_lost(); }

void dm_focus_set(int st); void dm_audio_suspend(int on); void guest_clock_freeze_for(int reason, int on); void dm_gfx_suspend(int on);
// ---- settings (the pause menu's), kept in the files folder as KEY=VALUE lines ----
static char g_root[512];
static _Atomic int g_set_overlay = 1, g_set_rumble = 1, g_set_fps;   // the gamepad shown; rumble and tap vibration; the frame rate counter (top left)
static _Atomic int g_set_opacity = 60;   // the gamepad's opacity, percent (OV_OPACITY_MIN..MAX: the menu's Opacity slider)
// the player's own layout (overlay_layout.h dm_overlay_custom): [0] the saved one (settings.txt ui= rows), [1] the layout editor's while it is open; per layout
// and control: x + 1, y + 1 in 1/10000 of the window's width and height (0 = the layout's place), the size factor in 1/1000 (0 = 1)
static _Atomic int g_cust[2][2][DM_AOV_N][3], g_cust_ver, g_edit;
static const char *const LAYOUT_NAMES[2] = {"tablet", "phone"};
static int ctl_named(const char *n) { for (int i = 0; i < OV_N; i++) if (!strcmp(OV_LAYOUTS[0][i].name, n)) return i; return -1; }
int dm_opmode(void); void dm_set_opmode(int m);   // hle_oe.c: 0 handheld, 1 docked; a change reaches the game as the Switch's operation mode notification
static void settings_load(void) {
    char p[600]; snprintf(p, sizeof p, "%s/settings.txt", g_root); FILE *f = fopen(p, "r"); char line[128];
    if (!f) return;
    while (fgets(line, sizeof line, f)) { int v; if (sscanf(line, "overlay=%d", &v) == 1) atomic_store(&g_set_overlay, v != 0); else if (sscanf(line, "rumble=%d", &v) == 1) atomic_store(&g_set_rumble, v != 0);
        else if (sscanf(line, "fps=%d", &v) == 1) atomic_store(&g_set_fps, v != 0);
        else if (sscanf(line, "docked=%d", &v) == 1) setenv("DM_OPMODE", v ? "1" : "0", 1);   // over env.txt's: the host reads it once, at its first query
        else if (sscanf(line, "opacity=%d", &v) == 1) atomic_store(&g_set_opacity, v < OV_OPACITY_MIN ? OV_OPACITY_MIN : v > OV_OPACITY_MAX ? OV_OPACITY_MAX : v);
        else if (!strncmp(line, "ui=", 3)) {   // ui=<layout>,<control>,<x>,<y>,<size>: x, y in 1/10000 of the window (-1 the layout's place), size in 1/1000
            char lay[16], name[32]; int x, y, z, l, i;
            if (sscanf(line + 3, "%15[^,],%31[^,],%d,%d,%d", lay, name, &x, &y, &z) != 5 || (i = ctl_named(name)) < 0) continue;
            for (l = 0; l < 2 && strcmp(lay, LAYOUT_NAMES[l]); l++) {}
            if (l == 2) continue;
            if (x >= 0 && x <= 10000 && y >= 0 && y <= 10000) { atomic_store(&g_cust[0][l][i][0], x + 1); atomic_store(&g_cust[0][l][i][1], y + 1); }
            if (z >= OVE_SCALE_MIN * 1000 && z <= OVE_SCALE_MAX * 1000) atomic_store(&g_cust[0][l][i][2], z);
        } else if (sscanf(line, "menu_x=%d", &v) == 1 && v >= 0 && v <= 10000) { int m = ctl_named("menu"); for (int l = 0; l < 2 && m >= 0; l++) atomic_store(&g_cust[0][l][m][0], v + 1); }   // before
        else if (sscanf(line, "menu_y=%d", &v) == 1 && v >= 0 && v <= 10000) { int m = ctl_named("menu"); for (int l = 0; l < 2 && m >= 0; l++) atomic_store(&g_cust[0][l][m][1], v + 1); }   // the editor
    }
    fclose(f);
}
static void settings_save(void) {
    char p[600], tmp[610]; snprintf(p, sizeof p, "%s/settings.txt", g_root); snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "w"); if (!f) { LOG("[RED] cannot write %s", tmp); return; }
    fprintf(f, "overlay=%d\nrumble=%d\ndocked=%d\nfps=%d\nopacity=%d\n", atomic_load(&g_set_overlay), atomic_load(&g_set_rumble), dm_opmode() != 0, atomic_load(&g_set_fps),
            atomic_load(&g_set_opacity));
    for (int l = 0; l < 2; l++) for (int i = 0; i < OV_N; i++) {
        _Atomic int *c = g_cust[0][l][i]; int x = atomic_load(&c[0]), y = atomic_load(&c[1]), z = atomic_load(&c[2]);
        if ((x && y) || z) fprintf(f, "ui=%s,%s,%d,%d,%d\n", LAYOUT_NAMES[l], OV_LAYOUTS[0][i].name, x && y ? x - 1 : -1, x && y ? y - 1 : -1, z ? z : 1000);
    }
    fclose(f); rename(tmp, p);
}
void dm_haptics_android_start(ANativeActivity *act, int on); void dm_haptics_android_tap(void); void dm_haptics_android_device(int id); void dm_haptics_android_set(int on);
void dm_haptics_suspend(unsigned reason, int suspended);
__attribute__((weak)) void dm_overlay_redraw(void) {}   // the backend presents the last frame again (the pause menu changed while the game is held)

// ---- holding the game: in the background and while the pause menu is open. Its clock reads wait (reason 1) and its audio goes to the silent sink ----
static _Atomic int g_running, g_bg, g_menu;
static pthread_mutex_t g_hold_mu = PTHREAD_MUTEX_INITIALIZER; static int g_held;
static _Atomic int g_tx_on;   // text entry open (front_text_input): the game is held as by the menu
static void hold_sync(void) {
    pthread_mutex_lock(&g_hold_mu);
    int want = atomic_load(&g_bg) || atomic_load(&g_menu) || atomic_load(&g_tx_on);
    if (want != g_held) { g_held = want; if (want) { dm_movie_hold(1); dm_audio_suspend(1); guest_clock_freeze_for(1, 1); } else { guest_clock_freeze_for(1, 0); dm_audio_suspend(0); dm_movie_hold(0); } }
    pthread_mutex_unlock(&g_hold_mu);
}
static void shake_sensor(int on);   // the accelerometer, off in the background (input, below)
static void go_background(void) { if (!atomic_load(&g_running) || atomic_exchange(&g_bg, 1)) return; dm_focus_set(2); dm_focus_set(3);   // through OutOfFocus, as a game sees HOME and then the suspension (front_mac.m does the same)
    hold_sync(); dm_gfx_suspend(1); shake_sensor(0); }
static void go_foreground(void) { if (!atomic_load(&g_running) || !atomic_exchange(&g_bg, 0)) return; dm_gfx_suspend(0); hold_sync(); dm_focus_set(2); shake_sensor(1); }
static void on_pause(ANativeActivity *a) { (void)a; go_background(); }
// Back in front, the game is out of focus until the window has the input focus again (Android sends it after onResume): without this it stayed OutOfFocus for good,
// and Blasphemous 2's Unity, which asks for focus notifications, kept its title paused and ignored every press after a trip to another app. A window losing focus while in
// front (a system dialog, the notification shade) is OutOfFocus as on the Switch's HOME overlay
static void on_window_focus(ANativeActivity *a, int focused) { (void)a; if (atomic_load(&g_running) && !atomic_load(&g_bg)) dm_focus_set(focused ? 1 : 2); }
static void on_resume(ANativeActivity *a) { (void)a; go_foreground(); }
static void on_destroy(ANativeActivity *a) { (void)a; _exit(0); }   // one game per process

// ---- input ----
static AInputQueue *g_queue; static ALooper *g_ilooper;
static uint64_t g_btn; static float g_ax[8]; static int g_pad_used;   // lx ly rx ry lt rt hatx haty
static uint64_t key_bit(int32_t k) {   // Nintendo layout by position: the bottom face button is B, the right one A (as the iOS front end maps)
    switch (k) {
    case AKEYCODE_BUTTON_A: return DM_BTN_B; case AKEYCODE_BUTTON_B: return DM_BTN_A; case AKEYCODE_BUTTON_X: return DM_BTN_Y; case AKEYCODE_BUTTON_Y: return DM_BTN_X;
    case AKEYCODE_BUTTON_L1: return DM_BTN_L; case AKEYCODE_BUTTON_R1: return DM_BTN_R; case AKEYCODE_BUTTON_L2: return DM_BTN_ZL; case AKEYCODE_BUTTON_R2: return DM_BTN_ZR;
    case AKEYCODE_BUTTON_THUMBL: return DM_BTN_LS; case AKEYCODE_BUTTON_THUMBR: return DM_BTN_RS; case AKEYCODE_BUTTON_START: return DM_BTN_PLUS; case AKEYCODE_BUTTON_SELECT: return DM_BTN_MINUS;
    case AKEYCODE_DPAD_UP: return DM_BTN_UP; case AKEYCODE_DPAD_DOWN: return DM_BTN_DOWN; case AKEYCODE_DPAD_LEFT: return DM_BTN_LEFT; case AKEYCODE_DPAD_RIGHT: return DM_BTN_RIGHT;
    default: return 0;
    }
}
// A keyboard (a tablet's, a Chromebook's, the Android emulator's window on a computer) plays as a controller in Ryujinx's default layout: Z A, X B, C X, V Y, E L, U R, Q ZL,
// O ZR, F and H the stick buttons, - and = minus and plus, the arrow keys the d-pad (key_bit), WASD the left stick and IJKL the right one, at full tilt. In the emulator a mouse
// is one finger, so the on-screen L and R could not be held together for Link's Awakening's "Press L+R"
static uint64_t kb_bit(int32_t k) {
    switch (k) {
    case AKEYCODE_Z: return DM_BTN_A; case AKEYCODE_X: return DM_BTN_B; case AKEYCODE_C: return DM_BTN_X; case AKEYCODE_V: return DM_BTN_Y;
    case AKEYCODE_E: return DM_BTN_L; case AKEYCODE_U: return DM_BTN_R; case AKEYCODE_Q: return DM_BTN_ZL; case AKEYCODE_O: return DM_BTN_ZR;
    case AKEYCODE_F: return DM_BTN_LS; case AKEYCODE_H: return DM_BTN_RS; case AKEYCODE_MINUS: return DM_BTN_MINUS; case AKEYCODE_EQUALS: case AKEYCODE_PLUS: return DM_BTN_PLUS;
    default: return 0;
    }
}
static uint32_t kb_stick(int32_t k) {   // bits: left stick x− x+ y− y+ (A D W S, y in Android's direction: W is −), then the right stick's (J L I K)
    switch (k) { case AKEYCODE_A: return 1; case AKEYCODE_D: return 2; case AKEYCODE_W: return 4; case AKEYCODE_S: return 8;
                 case AKEYCODE_J: return 16; case AKEYCODE_L: return 32; case AKEYCODE_I: return 64; case AKEYCODE_K: return 128; default: return 0; }
}
static uint32_t g_kstick;   // the stick keys held
static float kb_axis(int sh, float pad) { uint32_t m = g_kstick >> sh & 3; return m ? (m & 2 ? 1.0f : 0.0f) - (m & 1 ? 1.0f : 0.0f) : pad; }   // keys held win over the controller's axis
static int32_t axis(float v) { if (v > -0.12f && v < 0.12f) return 0; return (int32_t)(v * DM_STICK_MAX); }
static uint64_t g_shake_btn;   // DM_BTN_SHAKE while the device is being shaken (below); input thread only
static void pad_publish(void) {
    uint64_t b = g_btn | g_shake_btn;
    if (g_ax[4] > 0.5f) b |= DM_BTN_ZL; if (g_ax[5] > 0.5f) b |= DM_BTN_ZR;
    if (g_ax[6] < -0.5f) b |= DM_BTN_LEFT; if (g_ax[6] > 0.5f) b |= DM_BTN_RIGHT; if (g_ax[7] < -0.5f) b |= DM_BTN_UP; if (g_ax[7] > 0.5f) b |= DM_BTN_DOWN;
    dm_pad_set(b, axis(kb_axis(0, g_ax[0])), -axis(kb_axis(2, g_ax[1])), axis(kb_axis(4, g_ax[2])), -axis(kb_axis(6, g_ax[3])));   // Android's y grows downward, the Switch's upward
}
// ---- the on-screen gamepad (overlay_layout.h): a pointer that goes down on a control keeps it until it lifts; every other pointer is the game's touch screen ----
static _Atomic int g_overlay = 1;   // shown until a physical controller is used, again at the next touch (when the player has it on)
static _Atomic int g_pads;          // controllers attached and used (pad_watch): while there is one, nothing of the gamepad is shown, the menu button neither
static _Atomic int g_pad_in = -1;   // the device that last sent a controller's input (handle_event), which pad_watch then counts
static _Atomic int g_knob[2][2];    // the sticks' knob offsets, in 1/10000 of the stick radius
static _Atomic int64_t g_press_t[16], g_menu_t, g_stick_t[2];   // when each overlay button (by DM_BTN_* bit), the menu button and each stick went down, 0 = not held
static uint64_t g_touch_btn;        // the overlay buttons held, as of the last touch event
static int64_t mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
int dm_overlay_visible(void) { return atomic_load(&g_pads) ? 0 : atomic_load(&g_set_overlay) && atomic_load(&g_overlay) ? 1 : 2; }
int dm_overlay_layout(void) {   // the phone's layout in a window at least OV_PHONE_ASPECT wide for its height (overlay_layout.h)
    static int last_w = -1, last_h = -1, phone;
    ANativeWindow *w = dm_front_window(); int ww = w ? ANativeWindow_getWidth(w) : 0, wh = w ? ANativeWindow_getHeight(w) : 0;
    if (ww != last_w || wh != last_h) { last_w = ww; last_h = wh; phone = wh > 0 && (float)ww / (float)wh >= OV_PHONE_ASPECT;
        if (wh > 0) LOG("[overlay] window %dx%d: the %s layout", ww, wh, phone ? "phone" : "tablet"); }
    return phone;
}
int dm_overlay_custom(int layout, int ctl, float *x, float *y, float *s) {   // (the editor's own while it is open)
    if (layout < 0 || layout > 1 || ctl < 0 || ctl >= OV_N) return 0;
    _Atomic int *c = g_cust[atomic_load(&g_edit) ? 1 : 0][layout][ctl]; int cx = atomic_load(&c[0]), cy = atomic_load(&c[1]), z = atomic_load(&c[2]);
    *s = z ? z / 1000.0f : 1;
    if (!cx || !cy) return 0;
    *x = (cx - 1) / 10000.0f; *y = (cy - 1) / 10000.0f; return 1;
}
int dm_overlay_custom_ver(void) { return atomic_load(&g_cust_ver); }
float dm_overlay_opacity(void) { return atomic_load(&g_set_opacity) / 100.0f; }
static void cust_place(int set, int l, int i, float x, float y) { atomic_store(&g_cust[set][l][i][0], 1 + (int)lroundf(x * 10000)); atomic_store(&g_cust[set][l][i][1], 1 + (int)lroundf(y * 10000)); }
int64_t dm_overlay_pressed(int ctl, uint64_t bit) {
    if (ctl < 0 || ctl >= OV_N) return 0;
    if (OV_CTLS[ctl].kind == OV_MENU) return atomic_load(&g_menu_t);
    if (OV_CTLS[ctl].kind == OV_STICK_L || OV_CTLS[ctl].kind == OV_STICK_R) return atomic_load(&g_stick_t[OV_CTLS[ctl].kind == OV_STICK_R]);
    if (OV_CTLS[ctl].kind == OV_BUTTON) bit = OV_CTLS[ctl].btn;
    return bit && bit < 1u << 16 ? atomic_load(&g_press_t[__builtin_ctzll(bit)]) : 0;
}
static int g_dpi;   // the display's density (AConfiguration), 0 = unknown
float dm_overlay_scale(void) {
    static int last_w = -1, last_h = -1; static float s = 1;
    ANativeWindow *w = dm_front_window(); int ww = w ? ANativeWindow_getWidth(w) : 0, h = w ? ANativeWindow_getHeight(w) : 0;
    if (h != last_h || ww != last_w) { last_w = ww; last_h = h; s = 1;   // (the width picks the layout, whose buttons are sized for its form factor)
        float face = 0.1f; const ov_ctl *L = OV_CTLS; for (int i = 0; i < OV_N; i++) if (L[i].kind == OV_BUTTON && L[i].btn == DM_BTN_A) face = L[i].w;   // A's width, in window heights
        if (h > 0 && g_dpi > 0 && g_dpi < 0xfffe) { float mm = face * (float)h / (float)g_dpi * 25.4f; s = OV_MIN_MM / mm; if (s < 1) s = 1; if (s > 1.45f) s = 1.45f; }   // (more and the face buttons touch)
        LOG("[overlay] window %dx%d, density %d dpi: face buttons %.1f mm, controls x%.2f", ww, h, g_dpi, h > 0 && g_dpi > 0 && g_dpi < 0xfffe ? face * h / g_dpi * 25.4f : 0, s); }
    return s;
}
void dm_overlay_knob(int stick, float *dx, float *dy) { *dx = atomic_load(&g_knob[stick][0]) / 10000.0f; *dy = atomic_load(&g_knob[stick][1]) / 10000.0f; }
static int g_ptr_ctl[32];   // pointer id → control index + 1 (0 = the touch screen)
static int ctl_at(float x, float y, float W, float H) {   // the control under (x, y) in pixels, -1 if none (in the layout editor: every control, shown or not)
    int best = -1, vis = atomic_load(&g_edit) ? 1 : dm_overlay_visible(), shown = vis == 1; float bd = 1e9f;
    if (!vis) return -1;   // a controller is attached: every touch is the game's
    for (int i = 0; i < OV_N; i++) {
        const ov_ctl *c = &OV_CTLS[i]; if (!shown && c->kind != OV_MENU) continue;   // hidden: only the menu button answers
        float b[4]; ov_box(c, W, H, b);
        float d = ov_sdf(c, b, x - b[0], y - b[1]), reach = ov_reach(c, b, H), m = fminf(b[2], b[3]);
        if (d < reach && (d + m) / (m + reach) < bd) { bd = (d + m) / (m + reach); best = i; }   // between two, the one the touch is deeper into, for its size
    }
    return best;
}
// dragging the menu button: the pointer holding it, whether it has moved past OV_DRAG_SLOP, where it went down and the finger's offset from the button's centre
static struct { int id, moved; float x0, y0, gx, gy; } g_drag = {-1};
static void menu_grab(int id, const ov_ctl *m, float x, float y, float W, float H) {
    float b[4]; ov_box(m, W, H, b);
    g_drag.id = id; g_drag.moved = 0; g_drag.x0 = x; g_drag.y0 = y; g_drag.gx = b[0] - x; g_drag.gy = b[1] - y;
}
static void menu_drag(const ov_ctl *m, float x, float y, float W, float H) {   // the button follows the finger once it has moved, kept whole on the screen
    if (!g_drag.moved && hypotf(x - g_drag.x0, y - g_drag.y0) < OV_DRAG_SLOP * H) return;
    g_drag.moved = 1;
    float b[4]; ov_box(m, W, H, b);
    float px = fminf(fmaxf(x + g_drag.gx, b[2]), W - b[2]), py = fminf(fmaxf(y + g_drag.gy, b[3]), H - b[3]); int l, i = ov_index(m, &l);
    cust_place(0, l, i, px / W, py / H); atomic_fetch_add(&g_cust_ver, 1);
}

// ---- controllers attached: InputDevice's list, read once a second through JNI (a code-less NativeActivity has no InputManager listener). A device counts
// when it is a gamepad or a joystick and not virtual (the tablet's own keys, touch screen and pen are neither), and once it has sent input: Xiaomi phones
// keep a system device "uinput-xiaomi" attached for good that says it is a gamepad and sends nothing, which hid the gamepad and its menu button (issue #22).
// While one is attached nothing of the gamepad is shown and every touch is the game's; its keys open the menu (Back, Mode, Menu: handle_event). When the
// last one goes, the gamepad comes back ----
static int jx(JNIEnv *E) { if (!(*E)->ExceptionCheck(E)) return 0; (*E)->ExceptionClear(E); return 1; }
static void *pad_watch(void *a) {
    JavaVM *vm = a; JNIEnv *E; pthread_setname_np(pthread_self(), "padwatch");
    if ((*vm)->AttachCurrentThread(vm, &E, NULL) != JNI_OK) { LOG("[RED] [input] cannot attach to the Java VM: attached controllers are not watched"); return NULL; }
    jclass cID = (*E)->FindClass(E, "android/view/InputDevice");
    jmethodID mIds = cID ? (*E)->GetStaticMethodID(E, cID, "getDeviceIds", "()[I") : NULL, mGet = cID ? (*E)->GetStaticMethodID(E, cID, "getDevice", "(I)Landroid/view/InputDevice;") : NULL;
    jmethodID mSrc = cID ? (*E)->GetMethodID(E, cID, "getSources", "()I") : NULL, mVirt = cID ? (*E)->GetMethodID(E, cID, "isVirtual", "()Z") : NULL;
    jmethodID mName = cID ? (*E)->GetMethodID(E, cID, "getName", "()Ljava/lang/String;") : NULL;
    jmethodID mVen = cID ? (*E)->GetMethodID(E, cID, "getVendorId", "()I") : NULL, mProd = cID ? (*E)->GetMethodID(E, cID, "getProductId", "()I") : NULL;
    if (jx(E) || !mIds || !mGet || !mSrc || !mVirt || !mName || !mVen || !mProd) { LOG("[RED] [input] InputDevice methods missing: attached controllers are not watched"); return NULL; }
    int prev[16], used[16], np = 0;
    for (;;) {
        int ids[16], on[16], n = 0, nu = 0, in = atomic_load(&g_pad_in);
        jintArray arr = (*E)->CallStaticObjectMethod(E, cID, mIds);
        if (!jx(E) && arr) {
            jsize len = (*E)->GetArrayLength(E, arr); jint *v = (*E)->GetIntArrayElements(E, arr, NULL);
            for (jsize i = 0; v && i < len && n < 16; i++) {
                jobject d = (*E)->CallStaticObjectMethod(E, cID, mGet, v[i]); if (jx(E) || !d) continue;
                jint src = (*E)->CallIntMethod(E, d, mSrc); jboolean virt = (*E)->CallBooleanMethod(E, d, mVirt);
                if (!jx(E) && !virt && ((src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD || (src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK)) {
                    int known = 0, was = 0; for (int k = 0; k < np; k++) if (prev[k] == v[i]) { known = 1; was = used[k]; }
                    ids[n] = v[i]; on[n] = was || v[i] == in; nu += on[n];
                    if (!known) {
                        jstring s = (*E)->CallObjectMethod(E, d, mName); const char *nm = !jx(E) && s ? (*E)->GetStringUTFChars(E, s, NULL) : NULL;
                        jint ven = (*E)->CallIntMethod(E, d, mVen), prod = (*E)->CallIntMethod(E, d, mProd); jx(E);
                        LOG("[input] controller attached: %s (device %d, sources 0x%x, %04x:%04x): the gamepad is hidden once it is used", nm ? nm : "?", v[i], src, ven, prod);
                        if (nm) (*E)->ReleaseStringUTFChars(E, s, nm);
                        if (s) (*E)->DeleteLocalRef(E, s);
                    }
                    if (on[n] && !was) LOG("[input] controller in use (device %d): the gamepad is hidden", v[i]);
                    n++;
                }
                (*E)->DeleteLocalRef(E, d);
            }
            if (v) (*E)->ReleaseIntArrayElements(E, arr, v, JNI_ABORT);
            (*E)->DeleteLocalRef(E, arr);
        }
        for (int k = 0; k < np; k++) { int still = 0; for (int i = 0; i < n; i++) still |= ids[i] == prev[k]; if (!still) LOG("[input] controller detached (device %d)", prev[k]); }
        if (atomic_exchange(&g_pads, nu) && !nu) {   // the last one went: the gamepad again, and the device's motor for rumble
            atomic_store(&g_overlay, 1); dm_haptics_android_device(-1);
            LOG("[input] no controller in use: the gamepad is shown");
        }
        memcpy(prev, ids, sizeof ids); memcpy(used, on, sizeof on); np = n;
        sleep(1);
    }
    return NULL;
}

// ---- the pause menu: the game is held while it is open; touches and a controller's keys go to it. Two tabs (overlay_layout.h OVM_TAB_ITEMS): the gamepad's
// settings (shown, opacity, the layout editor, rumble) and the game's (the frame rate counter, the mode); Resume under them ----
static _Atomic int g_menu_hi = -1, g_menu_tab;   // the highlighted row (a controller's selection, or the row a finger is on), -1 none; the open tab
static int g_menu_ptr = -1, g_menu_row = -2;   // the pointer pressing in the menu, and the row it went down on (-3 the menu button)
static _Atomic int g_tx_ver;   // the text entry's edit count: the panel is drawn again when it changes
static _Atomic int g_ed_st;    // the layout editor's state for dm_menu_state (overlay_layout.h OVE_ST)
int dm_menu_state(void) {
    if (atomic_load(&g_tx_on)) return 1 << 16 | (atomic_load(&g_tx_ver) & 0xffff);   // the text entry's panel (gfx_vk_present.c ov_text_panel)
    if (!atomic_load(&g_menu)) return -1;
    if (atomic_load(&g_edit)) return OVE_ST | atomic_load(&g_ed_st);
    return atomic_load(&g_set_overlay) | atomic_load(&g_set_rumble) << 1 | (dm_opmode() != 0) << 2 | atomic_load(&g_set_fps) << 3 | (atomic_load(&g_menu_hi) + 1) << 4 |
           atomic_load(&g_menu_tab) << 8 | atomic_load(&g_set_opacity) << 9;
}
int dm_overlay_fps(void) { return atomic_load(&g_set_fps) && !atomic_load(&g_menu); }   // gfx_vk_present.c draws the counter (not while the menu holds the game)
static void menu_open(int hi) {
    if (!atomic_load(&g_running) || atomic_exchange(&g_menu, 1)) return;
    atomic_store(&g_menu_hi, hi); g_menu_ptr = -1;
    dm_input_set(0, 0, 0, 0, 0); dm_touch_set(NULL, 0); g_touch_btn = 0;   // nothing stays pressed in the game
    for (int i = 0; i < 16; i++) atomic_store(&g_press_t[i], 0);
    for (int s = 0; s < 2; s++) { atomic_store(&g_stick_t[s], 0); for (int a = 0; a < 2; a++) atomic_store(&g_knob[s][a], 0); }
    if (g_pad_used) { g_btn = 0; memset(g_ax, 0, sizeof g_ax); pad_publish(); }
    for (int i = 0; i < 32; i++) g_ptr_ctl[i] = 0;
    dm_haptics_suspend(4, 1);   // (bits 1 and 2 are the iOS front end's reasons)
    hold_sync(); dm_overlay_redraw();
    LOG("[menu] opened: the game is paused");
}
static void ed_cancel(void);
static void menu_close(void) {
    if (atomic_load(&g_edit)) { ed_cancel(); return; }   // the editor first: its changes are kept or thrown away before the game goes on
    if (!atomic_exchange(&g_menu, 0)) return;
    g_menu_ptr = -1; atomic_store(&g_menu_t, 0);
    hold_sync(); dm_haptics_suspend(4, 0); dm_overlay_redraw();
    LOG("[menu] closed: the game goes on (gamepad %s, opacity %d%%, rumble %s, frame rate %s, %s)", atomic_load(&g_set_overlay) ? "on" : "off", atomic_load(&g_set_opacity),
        atomic_load(&g_set_rumble) ? "on" : "off", atomic_load(&g_set_fps) ? "shown" : "hidden", dm_opmode() ? "docked" : "handheld");
}
static void menu_opacity(int v) {   // the slider: a new value is drawn at once and saved when the finger lifts (menu_touch) or with each controller step
    v = v < OV_OPACITY_MIN ? OV_OPACITY_MIN : v > OV_OPACITY_MAX ? OV_OPACITY_MAX : v;
    if (atomic_exchange(&g_set_opacity, v) != v) { dm_haptics_android_tap(); dm_overlay_redraw(); }
}
static void menu_tab(int t) { t = (t % OVM_TABS + OVM_TABS) % OVM_TABS; if (atomic_exchange(&g_menu_tab, t) != t) dm_haptics_android_tap(); dm_overlay_redraw(); }
static void ed_open(void);
static void menu_do(int item) {
    if (item == OVM_OVERLAY) { atomic_store(&g_set_overlay, !atomic_load(&g_set_overlay)); atomic_store(&g_overlay, 1); settings_save(); }
    else if (item == OVM_RUMBLE) { int on = !atomic_load(&g_set_rumble); atomic_store(&g_set_rumble, on); dm_haptics_android_set(on); settings_save(); }
    else if (item == OVM_FPS) { atomic_store(&g_set_fps, !atomic_load(&g_set_fps)); settings_save(); }
    else if (item == OVM_MODE) { dm_set_opmode(dm_opmode() ? 0 : 1); settings_save(); }   // the game takes it when it goes on
    else if (item == OVM_TAB) { menu_tab(atomic_load(&g_menu_tab) + 1); return; }
    else if (item == OVM_EDIT) { dm_haptics_android_tap(); ed_open(); return; }
    else if (item == OVM_OPACITY) return;   // (the slider is dragged, or stepped with left and right)
    if (item >= 0 && item < OVM_N) dm_haptics_android_tap();
    if (item == OVM_RESUME) menu_close(); else dm_overlay_redraw();
}
static void menu_touch(AInputEvent *e, float W, float H) {   // a press on a row does it when it lifts there; a tap outside the panel or on the menu button closes
    int act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK, ai = AMotionEvent_getAction(e) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    int i = act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_UP ? 0 : ai, id = AMotionEvent_getPointerId(e, (size_t)i) & 31;
    float x = AMotionEvent_getX(e, (size_t)i), y = AMotionEvent_getY(e, (size_t)i), p[4]; ov_menu_rect(W, H, p);
    int c = ctl_at(x, y, W, H), row = c >= 0 && OV_CTLS[c].kind == OV_MENU ? -3 : ov_menu_row_at(x, y, W, H), tab = atomic_load(&g_menu_tab);
    int item = row >= 0 ? ov_menu_item(tab, row) : -1;
    if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        if (g_menu_ptr >= 0) return;
        g_menu_ptr = id; g_menu_row = row;
        if (item == OVM_TAB) menu_tab(ov_menu_tab_at(x, p));   // a tab opens as the finger touches it
        else if (item == OVM_OPACITY) menu_opacity(ov_menu_track_value(x, p, row));
        if (row >= 0) { atomic_store(&g_menu_hi, item >= 0 ? row : -1); dm_overlay_redraw(); }
    } else if (act == AMOTION_EVENT_ACTION_MOVE && g_menu_ptr >= 0 && g_menu_row >= 0) {   // the slider follows the finger; another row's highlight follows it off the row and back
        for (size_t k = 0; k < AMotionEvent_getPointerCount(e); k++) if ((AMotionEvent_getPointerId(e, k) & 31) == g_menu_ptr) {
            float kx = AMotionEvent_getX(e, k), ky = AMotionEvent_getY(e, k);
            if (ov_menu_item(tab, g_menu_row) == OVM_OPACITY) { menu_opacity(ov_menu_track_value(kx, p, g_menu_row)); continue; }
            int r = ov_menu_row_at(kx, ky, W, H), hi = r == g_menu_row && ov_menu_item(tab, r) >= 0 ? r : -1;
            if (atomic_exchange(&g_menu_hi, hi) != hi) dm_overlay_redraw();
        }
    } else if ((act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_POINTER_UP) && id == g_menu_ptr) {
        int down = g_menu_row; g_menu_ptr = -1; atomic_store(&g_menu_hi, -1);
        if (down >= 0 && ov_menu_item(tab, down) == OVM_OPACITY) { settings_save(); dm_overlay_redraw(); }
        else if (row == down && item >= 0 && item != OVM_TAB) menu_do(item);
        else if (row == down && (row == -2 || row == -3)) { dm_haptics_android_tap(); menu_close(); }
        else dm_overlay_redraw();
    } else if (act == AMOTION_EVENT_ACTION_CANCEL) { g_menu_ptr = -1; atomic_store(&g_menu_hi, -1); dm_overlay_redraw(); }
}
static void ed_key(uint64_t bit);
static void menu_key(uint64_t bit) {   // a controller key pressed while the menu is open: up and down go through the rows, left and right change the tab or the
    if (atomic_load(&g_edit)) { ed_key(bit); return; }   // opacity on their rows, L and R change the tab anywhere, A does the row, B and + close
    int hi = atomic_load(&g_menu_hi), tab = atomic_load(&g_menu_tab), item = hi >= 0 ? ov_menu_item(tab, hi) : -1;
    if (bit == DM_BTN_UP || bit == DM_BTN_DOWN) {
        for (int k = 0; k < OVM_ROWS; k++) { hi = hi < 0 ? (bit == DM_BTN_UP ? OVM_ROWS - 1 : 1) : (hi + (bit == DM_BTN_UP ? OVM_ROWS - 1 : 1)) % OVM_ROWS; if (ov_menu_item(tab, hi) >= 0) break; }
        atomic_store(&g_menu_hi, hi); dm_overlay_redraw();
    } else if ((bit == DM_BTN_LEFT || bit == DM_BTN_RIGHT) && item == OVM_OPACITY) { menu_opacity(atomic_load(&g_set_opacity) + (bit == DM_BTN_LEFT ? -5 : 5)); settings_save(); }
    else if ((bit == DM_BTN_LEFT || bit == DM_BTN_RIGHT) && item == OVM_TAB) menu_tab(tab + (bit == DM_BTN_LEFT ? -1 : 1));
    else if (bit == DM_BTN_L || bit == DM_BTN_ZL || bit == DM_BTN_R || bit == DM_BTN_ZR) {
        menu_tab(tab + (bit == DM_BTN_L || bit == DM_BTN_ZL ? -1 : 1));
        if (hi > 0 && ov_menu_item(atomic_load(&g_menu_tab), hi) < 0) atomic_store(&g_menu_hi, OVM_ROWS - 1);   // (the other tab has fewer rows)
    }
    else if (bit == DM_BTN_A) menu_do(hi < 0 ? OVM_RESUME : item);
    else if (bit == DM_BTN_B || bit == DM_BTN_PLUS) menu_close();
}

// ---- the layout editor (overlay_layout.h OVE_*): opened from the menu, the game stays held. A finger on a control chooses it and drags it; a second finger
// pinches it bigger or smaller; - and + resize the chosen control, or every control when none is chosen; Reset puts the layout's places and sizes back;
// Save keeps the editor's layout (settings.txt, the next start too) and Cancel throws it away, asking first when something changed. Either goes back to the
// menu. A controller: + saves, B cancels; in the prompt A throws away and B goes back to editing. It edits the layout in use (tablet or phone) ----
static struct { int ptr, ptr2, sel, held, prompt, dirty, bdown, bar; float gx, gy, d0, s0; } g_ed = {-1, -1, -1, -1};   // bar: ptr went down on the toolbar or prompt
static void ed_publish(void) { atomic_store(&g_ed_st, g_ed.prompt | g_ed.dirty << 1 | (g_ed.held + 1) << 4 | (g_ed.sel + 1) << 8); dm_overlay_redraw(); }
static void ed_open(void) {
    for (int l = 0; l < 2; l++) for (int i = 0; i < OV_N; i++) for (int k = 0; k < 3; k++) atomic_store(&g_cust[1][l][i][k], atomic_load(&g_cust[0][l][i][k]));
    g_ed.ptr = g_ed.ptr2 = g_ed.sel = g_ed.held = g_ed.bdown = -1; g_ed.prompt = g_ed.dirty = g_ed.bar = 0; g_menu_ptr = -1;
    atomic_store(&g_edit, 1); atomic_fetch_add(&g_cust_ver, 1); ed_publish();
    LOG("[overlay] layout editor opened (%s layout)", LAYOUT_NAMES[dm_overlay_layout()]);
}
static void ed_close(int save) {
    if (save) for (int l = 0; l < 2; l++) for (int i = 0; i < OV_N; i++) for (int k = 0; k < 3; k++) atomic_store(&g_cust[0][l][i][k], atomic_load(&g_cust[1][l][i][k]));
    atomic_store(&g_edit, 0); atomic_fetch_add(&g_cust_ver, 1); g_menu_ptr = -1; atomic_store(&g_menu_hi, -1);
    if (save) settings_save();
    dm_haptics_android_tap(); dm_overlay_redraw();
    LOG("[overlay] layout editor closed: %s", save ? "the layout is saved" : g_ed.dirty ? "the changes are thrown away" : "no changes");
}
static void ed_cancel(void) { if (g_ed.dirty && !g_ed.prompt) { g_ed.prompt = 1; g_ed.held = -1; dm_haptics_android_tap(); ed_publish(); } else if (!g_ed.prompt) ed_close(0); }
static float ed_size(int l, int i) { int z = atomic_load(&g_cust[1][l][i][2]); return z ? z / 1000.0f : 1; }
static void ed_resize(int l, int i, float s) { s = fminf(fmaxf(s, OVE_SCALE_MIN), OVE_SCALE_MAX); atomic_store(&g_cust[1][l][i][2], (int)lroundf(s * 1000)); g_ed.dirty = 1; }
static void ed_do(int b) {
    int l = dm_overlay_layout();
    if (b == OVE_SMALLER || b == OVE_BIGGER) { float f = b == OVE_BIGGER ? 1.1f : 1 / 1.1f; for (int i = 0; i < OV_N; i++) if (g_ed.sel < 0 || i == g_ed.sel) ed_resize(l, i, ed_size(l, i) * f); }
    else if (b == OVE_RESET) { for (int i = 0; i < OV_N; i++) for (int k = 0; k < 3; k++) atomic_store(&g_cust[1][l][i][k], 0); g_ed.sel = -1; g_ed.dirty = 1; }
    else if (b == OVE_CANCEL) { ed_cancel(); return; }
    else if (b == OVE_SAVE) { ed_close(1); return; }
    else if (b == OVE_DISCARD) { ed_close(0); return; }
    else if (b == OVE_KEEP) g_ed.prompt = 0;
    dm_haptics_android_tap(); atomic_fetch_add(&g_cust_ver, 1); ed_publish();
}
static void ed_key(uint64_t bit) {
    if (g_ed.prompt) { if (bit == DM_BTN_A) ed_do(OVE_DISCARD); else if (bit == DM_BTN_B) ed_do(OVE_KEEP); return; }
    if (bit == DM_BTN_PLUS) ed_do(OVE_SAVE); else if (bit == DM_BTN_B) ed_do(OVE_CANCEL);
}
static void ed_drag(float x, float y, float W, float H) {   // the chosen control's centre follows the finger, kept on the screen
    int l = dm_overlay_layout(); float b[4]; ov_box(&OV_LAYOUTS[l][g_ed.sel], W, H, b);
    float px = fminf(fmaxf(x + g_ed.gx, fminf(b[2], W / 2)), W - fminf(b[2], W / 2)), py = fminf(fmaxf(y + g_ed.gy, fminf(b[3], H / 2)), H - fminf(b[3], H / 2));
    cust_place(1, l, g_ed.sel, px / W, py / H); g_ed.dirty = 1;
}
static void ed_touch(AInputEvent *e, float W, float H) {   // the prompt's buttons, else the toolbar's, else the controls
    int act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK, ai = AMotionEvent_getAction(e) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    int i = act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_UP ? 0 : ai, id = AMotionEvent_getPointerId(e, (size_t)i) & 31, l = dm_overlay_layout();
    float x = AMotionEvent_getX(e, (size_t)i), y = AMotionEvent_getY(e, (size_t)i), tb[4], pr[4]; ov_ed_rect(W, H, tb); ov_ed_prompt_rect(W, H, pr);
    int at = g_ed.prompt ? ov_ed_button_at(x, y, pr, OVE_DISCARD, 2) : ov_ed_button_at(x, y, tb, 0, OVE_N);   // a button under the finger
    int on_bar = !g_ed.prompt && x >= tb[0] && x < tb[0] + tb[2] && y >= tb[1] && y < tb[1] + tb[3];
    if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        if (g_ed.prompt || on_bar) {   // a button: done when the finger lifts on it
            if (g_ed.ptr < 0) { g_ed.bdown = at; g_ed.ptr = id; g_ed.held = at; g_ed.bar = 1; ed_publish(); }   // (the chosen control stays chosen for - and +)
            return;
        }
        if (g_ed.ptr >= 0 && !g_ed.bar && g_ed.sel >= 0 && g_ed.ptr2 < 0) {   // a second finger: the pinch, from the two fingers' distance now
            for (size_t k = 0; k < AMotionEvent_getPointerCount(e); k++) if ((AMotionEvent_getPointerId(e, k) & 31) == g_ed.ptr)
                g_ed.d0 = hypotf(x - AMotionEvent_getX(e, k), y - AMotionEvent_getY(e, k));
            g_ed.ptr2 = id; g_ed.s0 = ed_size(l, g_ed.sel); return;
        }
        if (g_ed.ptr >= 0) return;
        int c = ctl_at(x, y, W, H); g_ed.sel = c; g_ed.ptr = c >= 0 ? id : -1; g_ed.bdown = -1; g_ed.bar = 0;   // a control is chosen (nothing: none is)
        if (c >= 0) { float b[4]; ov_box(&OV_LAYOUTS[l][c], W, H, b); g_ed.gx = b[0] - x; g_ed.gy = b[1] - y; dm_haptics_android_tap(); }
        ed_publish();
    } else if (act == AMOTION_EVENT_ACTION_MOVE) {
        float px[2] = {0, 0}, py[2] = {0, 0}; int got = 0;
        for (size_t k = 0; k < AMotionEvent_getPointerCount(e); k++) {
            int pid = AMotionEvent_getPointerId(e, k) & 31, w = pid == g_ed.ptr ? 0 : pid == g_ed.ptr2 ? 1 : -1; if (w < 0) continue;
            px[w] = AMotionEvent_getX(e, k); py[w] = AMotionEvent_getY(e, k); got |= 1 << w;
        }
        if (g_ed.ptr >= 0 && g_ed.bar) {   // on a button: it stays lit while the finger is on it
            if (got & 1) { int a = g_ed.prompt ? ov_ed_button_at(px[0], py[0], pr, OVE_DISCARD, 2) : ov_ed_button_at(px[0], py[0], tb, 0, OVE_N), h = a == g_ed.bdown ? a : -1;
                if (h != g_ed.held) { g_ed.held = h; ed_publish(); } }
            return;
        }
        if (g_ed.sel < 0 || g_ed.ptr < 0) return;
        if (g_ed.ptr2 >= 0 && got == 3 && g_ed.d0 > 1) ed_resize(l, g_ed.sel, g_ed.s0 * hypotf(px[0] - px[1], py[0] - py[1]) / g_ed.d0);
        else if (got & 1) ed_drag(px[0], py[0], W, H);
        else return;
        ed_publish();
    } else if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_POINTER_UP || act == AMOTION_EVENT_ACTION_CANCEL) {
        if (id == g_ed.ptr2 && act != AMOTION_EVENT_ACTION_CANCEL) { g_ed.ptr2 = -1; atomic_fetch_add(&g_cust_ver, 1); return; }
        if (id != g_ed.ptr && act != AMOTION_EVENT_ACTION_CANCEL) return;
        int b = g_ed.bar ? g_ed.bdown : -1; g_ed.ptr = g_ed.ptr2 = g_ed.bdown = g_ed.held = -1; g_ed.bar = 0; atomic_fetch_add(&g_cust_ver, 1);
        if (b >= 0 && b == at && act != AMOTION_EVENT_ACTION_CANCEL) ed_do(b); else ed_publish();
    }
}

// ---- text entry: the game's software keyboard (nn::swkbd, hle_swkbd.c → front_text_input), as an on-screen keyboard of the overlay ----
// The game thread that asks waits here while the player types; the game is held as by the menu and the last frame is shown with the entry's panel and keys
// (overlay_layout.h ov_kb_*, drawn by gfx_vk_present.c ov_text_panel from dm_text_entry). Touch: tap the keys. Controller: the d-pad moves the highlight,
// A presses it, B erases, + accepts, − cancels. A physical keyboard types (letters, digits, space, punctuation as ASCII, the overlay's font), Del erases,
// Enter accepts. Back cancels (the game keeps its initial text). Android's own keyboard is not used: a code-less NativeActivity's content view is no text
// editor, so the system does not show one for it, and asking for one routed the key events to it
static pthread_mutex_t g_tx_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_tx_cv = PTHREAD_COND_INITIALIZER;
static struct { char title[128], text[256]; int n, max, done, ok, sel, shift, ptr; } g_tx;
int dm_text_entry(char *title, int tcap, char *text, int xcap, int *sel, int *shift) {   // the panel's content (the present thread); 0 when no entry is open
    if (!atomic_load(&g_tx_on)) return 0;
    pthread_mutex_lock(&g_tx_mu);
    snprintf(title, (size_t)tcap, "%s", g_tx.title); snprintf(text, (size_t)xcap, "%s", g_tx.text); *sel = g_tx.sel; *shift = g_tx.shift;
    pthread_mutex_unlock(&g_tx_mu);
    return 1;
}
static void tx_changed(void) { atomic_fetch_add(&g_tx_ver, 1); dm_overlay_redraw(); }
static void tx_add(int c) {   // the caller holds g_tx_mu; a capital ends the shift, as on a phone
    if (!c || g_tx.n >= g_tx.max) return;
    g_tx.text[g_tx.n++] = (char)c; g_tx.text[g_tx.n] = 0;
    if (c >= 'A' && c <= 'Z') g_tx.shift = 0;
}
static void tx_finish(int ok);
static void tx_press(int i) {   // an on-screen key
    if (i == OVK_OK) { tx_finish(1); return; }
    pthread_mutex_lock(&g_tx_mu);
    if (i == OVK_SHIFT) g_tx.shift = !g_tx.shift;
    else if (i == OVK_SPACE) tx_add(' ');
    else if (i == OVK_DEL) { if (g_tx.n) g_tx.text[--g_tx.n] = 0; }
    else tx_add(ov_kb_char(i, g_tx.shift));
    pthread_mutex_unlock(&g_tx_mu);
    dm_haptics_android_tap(); tx_changed();
}
static int tx_char(int32_t k, int32_t meta) {   // the ASCII character of a key, 0 for none
    int sh = (meta & AMETA_SHIFT_ON) != 0;
    if (k >= AKEYCODE_A && k <= AKEYCODE_Z) return (sh ^ ((meta & AMETA_CAPS_LOCK_ON) != 0) ? 'A' : 'a') + (k - AKEYCODE_A);
    if (k >= AKEYCODE_0 && k <= AKEYCODE_9) return sh ? ")!@#$%^&*("[k - AKEYCODE_0] : '0' + (k - AKEYCODE_0);
    switch (k) {
    case AKEYCODE_SPACE: return ' ';
    case AKEYCODE_COMMA: return sh ? '<' : ','; case AKEYCODE_PERIOD: return sh ? '>' : '.'; case AKEYCODE_MINUS: return sh ? '_' : '-';
    case AKEYCODE_EQUALS: return sh ? '+' : '='; case AKEYCODE_SLASH: return sh ? '?' : '/'; case AKEYCODE_APOSTROPHE: return sh ? '"' : '\'';
    case AKEYCODE_SEMICOLON: return sh ? ':' : ';'; case AKEYCODE_AT: return '@'; case AKEYCODE_PLUS: return '+'; case AKEYCODE_STAR: return '*'; case AKEYCODE_POUND: return '#';
    case AKEYCODE_LEFT_BRACKET: return sh ? '{' : '['; case AKEYCODE_RIGHT_BRACKET: return sh ? '}' : ']';
    default: return 0;
    }
}
static void tx_finish(int ok) {
    pthread_mutex_lock(&g_tx_mu); g_tx.ok = ok; g_tx.done = 1; pthread_cond_broadcast(&g_tx_cv); pthread_mutex_unlock(&g_tx_mu);
}
static int tx_key(AInputEvent *e) {   // every key goes to the entry while it is open
    int32_t k = AKeyEvent_getKeyCode(e);
    if (AKeyEvent_getAction(e) != AKEY_EVENT_ACTION_DOWN) return 1;
    uint64_t bit = key_bit(k);
    if (k == AKEYCODE_ENTER || k == AKEYCODE_NUMPAD_ENTER || bit == DM_BTN_PLUS) { tx_finish(1); return 1; }
    if (k == AKEYCODE_BACK || k == AKEYCODE_ESCAPE || bit == DM_BTN_MINUS) { tx_finish(0); return 1; }
    if (bit == DM_BTN_A) { int s; pthread_mutex_lock(&g_tx_mu); s = g_tx.sel; pthread_mutex_unlock(&g_tx_mu); tx_press(s); return 1; }
    pthread_mutex_lock(&g_tx_mu);
    int dx = bit == DM_BTN_LEFT ? -1 : bit == DM_BTN_RIGHT ? 1 : 0, dy = bit == DM_BTN_UP ? -1 : bit == DM_BTN_DOWN ? 1 : 0;
    if (dx || dy) g_tx.sel = ov_kb_move(g_tx.sel, dx, dy);
    else if (k == AKEYCODE_DEL || bit == DM_BTN_B) { if (g_tx.n) g_tx.text[--g_tx.n] = 0; }
    else if (bit == DM_BTN_Y) g_tx.shift = !g_tx.shift;
    else tx_add(tx_char(k, AKeyEvent_getMetaState(e)));
    pthread_mutex_unlock(&g_tx_mu);
    tx_changed();
    return 1;
}
static void tx_touch(AInputEvent *e, float W, float H) {   // a key is pressed when the finger that went down on it lifts there
    int act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK, ai = AMotionEvent_getAction(e) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    int i = act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_UP ? 0 : ai;
    float p[4]; ov_menu_rect(W, H, p);
    int key = ov_kb_at(AMotionEvent_getX(e, (size_t)i), AMotionEvent_getY(e, (size_t)i), p);
    if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        pthread_mutex_lock(&g_tx_mu); g_tx.ptr = key; if (key >= 0) g_tx.sel = key; pthread_mutex_unlock(&g_tx_mu);
        if (key >= 0) tx_changed();
    } else if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_POINTER_UP) {
        int down; pthread_mutex_lock(&g_tx_mu); down = g_tx.ptr; g_tx.ptr = -1; pthread_mutex_unlock(&g_tx_mu);
        if (key >= 0 && key == down) tx_press(key);
    }
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!out || out_cap <= 0) return -1;
    if (!g_act || !dm_front_window()) return -1;   // the adb shell executable, or no window yet: nothing is shown
    pthread_mutex_lock(&g_tx_mu);
    const char *t = title && *title ? title : guide && *guide ? guide : sub && *sub ? sub : "Enter text";
    snprintf(g_tx.title, sizeof g_tx.title, "%s", t);
    g_tx.n = 0; g_tx.text[0] = 0;
    for (const char *s = initial ? initial : ""; *s && g_tx.n < (int)sizeof g_tx.text - 1; s++) if ((unsigned char)*s >= 32 && (unsigned char)*s < 127) g_tx.text[g_tx.n++] = *s;   // ASCII of the initial text
    g_tx.text[g_tx.n] = 0;
    g_tx.max = max_chars > 0 && max_chars < (int)sizeof g_tx.text - 1 ? max_chars : (int)sizeof g_tx.text - 1;
    if (g_tx.max > out_cap - 1) g_tx.max = out_cap - 1;
    g_tx.done = g_tx.ok = 0; g_tx.sel = 11; g_tx.shift = g_tx.n == 0; g_tx.ptr = -1;   // the highlight on q; a capital first, as on a phone
    pthread_mutex_unlock(&g_tx_mu);
    dm_input_set(0, 0, 0, 0, 0); dm_touch_set(NULL, 0); g_touch_btn = 0;   // nothing stays pressed in the game
    atomic_store(&g_tx_on, 1); hold_sync(); tx_changed();
    LOG("[text] entry open: \"%s\" (at most %d characters)", g_tx.title, g_tx.max);
    pthread_mutex_lock(&g_tx_mu);
    while (!g_tx.done) pthread_cond_wait(&g_tx_cv, &g_tx_mu);
    int ok = g_tx.ok; if (ok) snprintf(out, (size_t)out_cap, "%s", g_tx.text);
    pthread_mutex_unlock(&g_tx_mu);
    atomic_store(&g_tx_on, 0); hold_sync(); dm_overlay_redraw();
    LOG("[text] entry %s", ok ? "accepted" : "cancelled");
    return ok;
}

__attribute__((weak)) void dm_picture_rect(float r[4]);   // gfx_vk_present.c
static void touch_event(AInputEvent *e) {
    ANativeWindow *w = dm_front_window(); float W = w ? (float)ANativeWindow_getWidth(w) : 1, H = w ? (float)ANativeWindow_getHeight(w) : 1;
    if (atomic_load(&g_tx_on)) { tx_touch(e, W, H); return; }   // the text entry's keys
    if (atomic_load(&g_menu)) { if (atomic_load(&g_edit)) ed_touch(e, W, H); else menu_touch(e, W, H); return; }
    int act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK, ai = AMotionEvent_getAction(e) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    size_t pc = AMotionEvent_getPointerCount(e);
    if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        int i = act == AMOTION_EVENT_ACTION_DOWN ? 0 : ai, id = AMotionEvent_getPointerId(e, (size_t)i) & 31;
        int c = ctl_at(AMotionEvent_getX(e, (size_t)i), AMotionEvent_getY(e, (size_t)i), W, H);
        g_ptr_ctl[id] = c + 1;
        if (c >= 0 && OV_CTLS[c].kind == OV_MENU) { atomic_store(&g_menu_t, mono_ns()); dm_haptics_android_tap(); menu_grab(id, &OV_CTLS[c], AMotionEvent_getX(e, (size_t)i), AMotionEvent_getY(e, (size_t)i), W, H); }
        if (!atomic_load(&g_overlay) && !atomic_load(&g_pads)) { atomic_store(&g_overlay, 1); dm_haptics_android_device(-1); }   // a touch brings the gamepad back (not over an attached controller)
    }
    uint64_t btn = 0; int32_t lx = 0, ly = 0, rx = 0, ry = 0; float kn[2][2] = {{0, 0}, {0, 0}}; int open = 0, held[2] = {0, 0};
    dm_touch t[16]; int n = 0;
    float pic[4] = {0, 0, 1, 1}; if (dm_picture_rect) dm_picture_rect(pic); if (pic[2] <= 0 || pic[3] <= 0) pic[2] = pic[3] = 1;   // gfx_vk_present.c: the picture inside the window
    for (size_t i = 0; i < pc; i++) {
        int id = AMotionEvent_getPointerId(e, i) & 31;
        int lifted = act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_CANCEL || (act == AMOTION_EVENT_ACTION_POINTER_UP && (int)i == ai);
        float x = AMotionEvent_getX(e, i), y = AMotionEvent_getY(e, i);
        int c = g_ptr_ctl[id] - 1;
        if (lifted) {
            if (c >= 0 && OV_CTLS[c].kind == OV_MENU) {   // a tap opens the menu; a drag leaves the button where it is
                atomic_store(&g_menu_t, 0);
                if (id == g_drag.id && g_drag.moved) { settings_save(); float mx = 0, my = 0, ms; dm_overlay_custom(dm_overlay_layout(), c, &mx, &my, &ms); LOG("[overlay] menu button moved to %.3f, %.3f", mx, my); }
                else open = act != AMOTION_EVENT_ACTION_CANCEL;
                if (id == g_drag.id) g_drag.id = -1;
            }
            g_ptr_ctl[id] = 0; continue;
        }
        if (c < 0) { if (n < 16) { t[n].id = (uint32_t)id & 15; t[n].x = (x / W - pic[0]) / pic[2]; t[n].y = (y / H - pic[1]) / pic[3]; n++; } continue; }   // relative to the game's picture
        const ov_ctl *k = &OV_CTLS[c];
        if (k->kind == OV_MENU) { if (id == g_drag.id) menu_drag(k, x, y, W, H); continue; }
        float b[4]; ov_box(k, W, H, b);
        float dx = (x - b[0]) / b[2], dy = (y - b[1]) / b[2], m = sqrtf(dx * dx + dy * dy);   // in the control's half widths (a stick's or the d-pad's radius)
        if (k->kind == OV_BUTTON) btn |= k->btn;
        else if (k->kind == OV_DPAD) {
            if (m > 0.25f) { float a = atan2f(dy, dx); int s = (int)lroundf(a / (3.14159265f / 4)) & 7;   // 8 directions, 0 = right, clockwise (y grows downward)
                static const uint64_t dir[8] = {DM_BTN_RIGHT, DM_BTN_RIGHT | DM_BTN_DOWN, DM_BTN_DOWN, DM_BTN_DOWN | DM_BTN_LEFT, DM_BTN_LEFT, DM_BTN_LEFT | DM_BTN_UP, DM_BTN_UP, DM_BTN_UP | DM_BTN_RIGHT};
                btn |= dir[s]; }
        } else {
            if (m > 1) { dx /= m; dy /= m; }
            kn[k->kind == OV_STICK_R][0] = dx; kn[k->kind == OV_STICK_R][1] = dy; held[k->kind == OV_STICK_R] = 1;   // the knob follows the finger (to the ring)
            int32_t sx = (int32_t)(dx * DM_STICK_MAX), sy = (int32_t)(-dy * DM_STICK_MAX);
            if (k->kind == OV_STICK_L) { lx = sx; ly = sy; } else { rx = sx; ry = sy; }
        }
    }
    uint64_t down = btn & ~g_touch_btn, up = g_touch_btn & ~btn; int64_t now = mono_ns(); g_touch_btn = btn;
    for (int b = 0; b < 16; b++) { if (down >> b & 1) atomic_store(&g_press_t[b], now); else if (up >> b & 1) atomic_store(&g_press_t[b], 0); }
    if (down) dm_haptics_android_tap();   // a tick for each button (or d-pad direction) the finger reaches
    for (int s = 0; s < 2; s++) for (int a = 0; a < 2; a++) atomic_store(&g_knob[s][a], (int)(kn[s][a] * 10000));
    for (int s = 0; s < 2; s++) if (!held[s]) atomic_store(&g_stick_t[s], 0); else if (!atomic_load(&g_stick_t[s])) atomic_store(&g_stick_t[s], now);
    dm_input_set(btn, lx, ly, rx, ry);
    dm_touch_set(t, n);
    if (open) menu_open(-1);
}
static int handle_event(AInputEvent *e) {
    int32_t src = AInputEvent_getSource(e), type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        if (atomic_load(&g_tx_on)) return tx_key(e);   // the text entry takes every key while it is open
        int32_t k = AKeyEvent_getKeyCode(e), down = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN;
        if (k == AKEYCODE_BACK || k == AKEYCODE_BUTTON_MODE || k == AKEYCODE_MENU) {   // the menu, both ways (Back would otherwise finish the activity, and the game with it)
            if (down && AKeyEvent_getRepeatCount(e) == 0) { if (atomic_load(&g_menu)) menu_close(); else menu_open(k == AKEYCODE_BACK ? -1 : 0); }
            return 1;
        }
        uint64_t bit = key_bit(k); uint32_t ks = 0;
        // a keyboard's letters (kb_bit): any key not from a controller (adb's `input keyevent` sends them with no source at all)
        if (!bit && (src & AINPUT_SOURCE_GAMEPAD) != AINPUT_SOURCE_GAMEPAD && !(ks = kb_stick(k))) bit = kb_bit(k);
        if ((ks || bit) && (src & AINPUT_SOURCE_GAMEPAD) != AINPUT_SOURCE_GAMEPAD) { static int said; if (!said++) LOG("[input] keyboard (device %d) played as a controller: WASD / IJKL sticks, Z X C V = A B X Y, E U = L R, Q O = ZL ZR, arrows d-pad, - = minus plus", AInputEvent_getDeviceId(e)); }
        if (ks) {   // a stick key: not for the pause menu, which the arrow keys move in
            atomic_store(&g_overlay, 0); g_pad_used = 1;
            if (!atomic_load(&g_menu)) { if (down) g_kstick |= ks; else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) g_kstick &= ~ks; dm_pad_players(1); pad_publish(); }
            return 1;
        }
        if (!bit) {   // the rest: the system's. A controller's key nothing maps is logged once, so a new controller's menu key can be found
            static uint8_t seen[64];
            if ((src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD && down && k >= 0 && k < 512 && !(seen[k >> 3] >> (k & 7) & 1)) {
                seen[k >> 3] |= (uint8_t)(1 << (k & 7)); LOG("[input] controller key %d (device %d): not mapped", k, AInputEvent_getDeviceId(e)); }
            return 0;
        }
        atomic_store(&g_overlay, 0); g_pad_used = 1; atomic_store(&g_pad_in, AInputEvent_getDeviceId(e)); dm_haptics_android_device(AInputEvent_getDeviceId(e));   // a controller is in use: the gamepad steps aside (until a touch)
        if (atomic_load(&g_menu)) { if (down && AKeyEvent_getRepeatCount(e) == 0) menu_key(bit); return 1; }
        if (down) g_btn |= bit; else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) g_btn &= ~bit;
        dm_pad_players(1); pad_publish(); return 1;
    }
    if (atomic_load(&g_tx_on) && ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK || (src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD)) return 1;   // sticks: not for the text entry
    if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK || (src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD) {
        g_pad_used = 1; atomic_store(&g_pad_in, AInputEvent_getDeviceId(e)); dm_haptics_android_device(AInputEvent_getDeviceId(e));
        if (atomic_load(&g_menu)) {   // the hat moves the selection
            static float hx, hy; float x = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0), y = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
            if (y < -0.5f && hy >= -0.5f) menu_key(DM_BTN_UP); else if (y > 0.5f && hy <= 0.5f) menu_key(DM_BTN_DOWN);
            if (x < -0.5f && hx >= -0.5f) menu_key(DM_BTN_LEFT); else if (x > 0.5f && hx <= 0.5f) menu_key(DM_BTN_RIGHT);
            hx = x; hy = y; return 1;
        }
        g_ax[0] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_X, 0); g_ax[1] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Y, 0);
        g_ax[2] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_Z, 0); g_ax[3] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RZ, 0);
        g_ax[4] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_LTRIGGER, 0); g_ax[5] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RTRIGGER, 0);
        g_ax[6] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0); g_ax[7] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        if (g_ax[4] == 0) g_ax[4] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_BRAKE, 0); if (g_ax[5] == 0) g_ax[5] = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_GAS, 0);
        dm_pad_players(1); pad_publish(); return 1;
    }
    if ((src & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) { touch_event(e); return 1; }
    return 0;
}
static int on_input(int fd, int events, void *data) {
    (void)fd; (void)events; (void)data;
    AInputEvent *e;
    while (g_queue && AInputQueue_getEvent(g_queue, &e) >= 0) {
        // no preDispatch: the app shows no soft keyboard (the text entry draws its own), and the IME's pre-dispatch swallowed a keyboard's letter keys
        AInputQueue_finishEvent(g_queue, e, handle_event(e));
    }
    return 1;
}
// ---- shaking the device: the controller is shaken (DM_BTN_SHAKE: its six-axis sensor, hle_hid.c; Super Mario Odyssey's cap throw), as the iOS front end does with the phone ----
// The accelerometer at 60 Hz on the input thread's looper (no permission needed): gravity is followed by a slow low-pass (about 0.8 s), and a sample whose acceleration beyond it
// exceeds DM_SHAKE_G (1.5 G by default; env.txt overrides) is a hit. The controller is shaken until 120 ms after the last hit: a flick is one shake (the host finishes it as a whole
// cycle) and shaking on keeps it going. It goes through player 1's controller path, so it adds to the on-screen gamepad and a controller alike. Off while the app is in the background
static ASensorEventQueue *g_sq; static const ASensor *g_accel;
static void shake_sensor(int on) {
    if (!g_sq || !g_accel) return;
    // the rate goes with the registration: enableSensor + setEventRate left it at the default 200 ms on the Y700 (dumpsys sensorservice), too slow for a flick
    if (on) ASensorEventQueue_registerSensor(g_sq, g_accel, 16667, 0); else ASensorEventQueue_disableSensor(g_sq, g_accel);
}
static int on_sensor(int fd, int events, void *data) {
    (void)fd; (void)events; (void)data;
    static double thr, gx, gy, gz, peak; static int64_t last = -1; static int init;
    if (!thr) { const char *e = getenv("DM_SHAKE_G"); thr = e && atof(e) > 0 ? atof(e) : 1.5; }
    ASensorEvent ev[16]; ssize_t n;
    while ((n = ASensorEventQueue_getEvents(g_sq, ev, 16)) > 0) for (ssize_t i = 0; i < n; i++) {
        if (ev[i].type != ASENSOR_TYPE_ACCELEROMETER) continue;
        double x = ev[i].acceleration.x / ASENSOR_STANDARD_GRAVITY, y = ev[i].acceleration.y / ASENSOR_STANDARD_GRAVITY, z = ev[i].acceleration.z / ASENSOR_STANDARD_GRAVITY;
        if (!init) { gx = x; gy = y; gz = z; init = 1; }
        double dx = x - gx, dy = y - gy, dz = z - gz, g = sqrt(dx * dx + dy * dy + dz * dz);
        gx += (x - gx) * 0.02; gy += (y - gy) * 0.02; gz += (z - gz) * 0.02;
        if (g > thr) { if (last < 0) peak = 0; last = ev[i].timestamp; peak = fmax(peak, g); }
        int on = last >= 0 && ev[i].timestamp - last < 120000000;
        if (!on && last >= 0) { LOG("front end: device shaken (peak %.1f G)", peak); last = -1; }
        uint64_t want = on ? DM_BTN_SHAKE : 0;
        if (want != g_shake_btn) { g_shake_btn = want; pad_publish(); }
    }
    return 1;
}
static void shake_start(void) {
    ASensorManager *m = ASensorManager_getInstanceForPackage(NULL);
    g_accel = m ? ASensorManager_getDefaultSensor(m, ASENSOR_TYPE_ACCELEROMETER) : NULL;
    if (!g_accel) { LOG("front end: no accelerometer, shaking the device does nothing"); return; }
    g_sq = ASensorManager_createEventQueue(m, g_ilooper, 2, on_sensor, NULL);
    const char *e = getenv("DM_SHAKE_G");
    LOG("front end: shaking the device shakes the controller (threshold %.2f G)", e && atof(e) > 0 ? atof(e) : 1.5);
    shake_sensor(!atomic_load(&g_bg));
}
static void *input_thread(void *a) {
    (void)a; g_ilooper = ALooper_prepare(0);
    shake_start();
    for (;;) ALooper_pollOnce(-1, NULL, NULL, NULL);
    return NULL;
}
static void on_queue_created(ANativeActivity *a, AInputQueue *q) { (void)a; g_queue = q; while (!g_ilooper) usleep(1000); AInputQueue_attachLooper(q, g_ilooper, 1, on_input, NULL); }
static void on_queue_destroyed(ANativeActivity *a, AInputQueue *q) { (void)a; AInputQueue_detachLooper(q); g_queue = NULL; }

// ---- the host ----
int main(int argc, char **argv);   // dm/host/main.c
static void *host_thread(void *a) {
    (void)a;
    char mods[600]; snprintf(mods, sizeof mods, "%s/mods", g_root);
    char *argv[] = {"dmhost", mods, NULL};
    atomic_store(&g_running, 1);
    int rc = main(2, argv);
    LOG("host returned %d", rc);
    _exit(rc);
}
static void env_file(const char *path) {   // KEY=VALUE per line (# comments): switches without rebuilding, as the iOS app's Documents/env.txt
    FILE *f = fopen(path, "r"); char line[1024];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line); while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '='); if (!eq) continue; *eq = 0; setenv(line, eq + 1, 1);
    }
    fclose(f);
}
__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *act, void *saved, size_t saved_size) {
    (void)saved; (void)saved_size;
    g_act = act;
    { AConfiguration *cfg = AConfiguration_new(); AConfiguration_fromAssetManager(cfg, act->assetManager); g_dpi = AConfiguration_getDensity(cfg); AConfiguration_delete(cfg); }
    act->callbacks->onNativeWindowCreated = on_window_created; act->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    act->callbacks->onInputQueueCreated = on_queue_created; act->callbacks->onInputQueueDestroyed = on_queue_destroyed;
    act->callbacks->onPause = on_pause; act->callbacks->onResume = on_resume; act->callbacks->onWindowFocusChanged = on_window_focus; act->callbacks->onDestroy = on_destroy;
    ANativeActivity_setWindowFlags(act, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    static int started; if (started++) return;   // a second onCreate (configuration change) keeps the running game
    snprintf(g_root, sizeof g_root, "%s", act->externalDataPath ? act->externalDataPath : act->internalDataPath);
    char p[700];
    snprintf(p, sizeof p, "%s/log.txt", g_root); char prev[720]; snprintf(prev, sizeof prev, "%s/log.prev.txt", g_root); rename(p, prev);
    if (!freopen(p, "w", stderr)) __android_log_print(ANDROID_LOG_ERROR, "dmhost", "cannot write %s", p);
    setvbuf(stderr, NULL, _IOLBF, 0);
    setenv("DM_ROOT", g_root, 1);
    snprintf(p, sizeof p, "%s/RomFS", g_root); setenv("DM_ROMFS", p, 0);
    snprintf(p, sizeof p, "%s/save", act->internalDataPath); mkdir(p, 0700); setenv("DM_SAVE", p, 0);
    snprintf(p, sizeof p, "%s/temp", act->internalDataPath); mkdir(p, 0700); setenv("DM_TEMP", p, 0);
    snprintf(p, sizeof p, "%s/shaders.dmsv", g_root); setenv("DM_VK_SHADERS", p, 0);
    snprintf(p, sizeof p, "%s/env.txt", g_root); env_file(p);
#ifdef DM_RELEASE
    unsetenv("DM_CTL");   // a release build takes no remote key presses (the control file is a development tool)
#endif
    settings_load(); dm_haptics_android_start(act, atomic_load(&g_set_rumble));
    { pthread_t w; if (!pthread_create(&w, NULL, pad_watch, act->vm)) pthread_detach(w); }
    LOG("NX2A host on Android: files %s, saves %s", g_root, act->internalDataPath);
    pthread_t t; pthread_create(&t, NULL, input_thread, NULL); pthread_detach(t);
    // The game's main thread: a 16 MB stack mapped here and handed over whole (pthread_attr_setstack), so the range pthread_getattr_np reports, which
    // svcQueryMemory gives the game as its main thread stack (hle_svc.c), is exactly mapped memory: IL2CPP's garbage collector scans that range from
    // its bottom, and with bionic's own stack the bottom of the reported range was not readable (SIGSEGV at main+0x2a59b4). A guard page sits below it.
    size_t ssz = 16 << 20, guard = 64 << 10;
    uint8_t *stk = mmap(NULL, ssz + guard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    pthread_attr_t at; pthread_attr_init(&at);
    if (stk != MAP_FAILED) { mprotect(stk, guard, PROT_NONE); pthread_attr_setstack(&at, stk + guard, ssz); } else pthread_attr_setstacksize(&at, ssz);
    if (pthread_create(&t, &at, host_thread, NULL)) LOG("[RED] cannot start the host thread"); else pthread_detach(t);
    pthread_attr_destroy(&at);
}
