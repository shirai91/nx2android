// Android port: the host front end (dm/host/front.h), in two forms.
//   dmhost executable (adb shell): headless, the guest runs on the calling thread as on the Mac without DM_WINDOW; no window, no input, no text entry.
//   libdmhost.so in the app (a NativeActivity, no Java): ANativeActivity_onCreate lays out the game's files in the app's external files folder
//   (/sdcard/Android/data/<package>/files: mods/ = the rewritten modules, RomFS/, shaders.dmsv; env.txt = KEY=VALUE switches; log.txt = the log), keeps saves in
//   internal storage, and starts the host on a thread of its own. The window goes to the Vulkan backend's swapchain (dm_front_window); game controllers
//   (KEYCODE_BUTTON_*, sticks, triggers, hat) feed player 1's pad; touches go to the game's touch screen; leaving and coming back pauses as on iOS.
#include "dm.h"
#include "front.h"
#include "dm_input.h"
#include "overlay_layout.h"
#include <math.h>
#include <time.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/window.h>
#include <android/native_window.h>
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
static _Atomic int g_mpos[2] = {-1, -1};   // where the player dragged the menu button: its centre in 1/10000 of the window's width and height, -1 the table's
int dm_opmode(void); void dm_set_opmode(int m);   // hle_oe.c: 0 handheld, 1 docked; a change reaches the game as the Switch's operation mode notification
static void settings_load(void) {
    char p[600]; snprintf(p, sizeof p, "%s/settings.txt", g_root); FILE *f = fopen(p, "r"); char line[128];
    if (!f) return;
    while (fgets(line, sizeof line, f)) { int v; if (sscanf(line, "overlay=%d", &v) == 1) atomic_store(&g_set_overlay, v != 0); else if (sscanf(line, "rumble=%d", &v) == 1) atomic_store(&g_set_rumble, v != 0);
        else if (sscanf(line, "fps=%d", &v) == 1) atomic_store(&g_set_fps, v != 0);
        else if (sscanf(line, "docked=%d", &v) == 1) setenv("DM_OPMODE", v ? "1" : "0", 1);   // over env.txt's: the host reads it once, at its first query
        else if (sscanf(line, "menu_x=%d", &v) == 1 && v >= 0 && v <= 10000) atomic_store(&g_mpos[0], v); else if (sscanf(line, "menu_y=%d", &v) == 1 && v >= 0 && v <= 10000) atomic_store(&g_mpos[1], v); }
    fclose(f);
}
static void settings_save(void) {
    char p[600], tmp[610]; snprintf(p, sizeof p, "%s/settings.txt", g_root); snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "w"); if (!f) { LOG("[RED] cannot write %s", tmp); return; }
    fprintf(f, "overlay=%d\nrumble=%d\ndocked=%d\nfps=%d\n", atomic_load(&g_set_overlay), atomic_load(&g_set_rumble), dm_opmode() != 0, atomic_load(&g_set_fps));
    if (atomic_load(&g_mpos[0]) >= 0 && atomic_load(&g_mpos[1]) >= 0) fprintf(f, "menu_x=%d\nmenu_y=%d\n", atomic_load(&g_mpos[0]), atomic_load(&g_mpos[1]));
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
static void go_background(void) { if (!atomic_load(&g_running) || atomic_exchange(&g_bg, 1)) return; dm_focus_set(3); hold_sync(); dm_gfx_suspend(1); }
static void go_foreground(void) { if (!atomic_load(&g_running) || !atomic_exchange(&g_bg, 0)) return; dm_gfx_suspend(0); hold_sync(); dm_focus_set(1); }
static void on_pause(ANativeActivity *a) { (void)a; go_background(); }
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
static int32_t axis(float v) { if (v > -0.12f && v < 0.12f) return 0; return (int32_t)(v * DM_STICK_MAX); }
static void pad_publish(void) {
    uint64_t b = g_btn;
    if (g_ax[4] > 0.5f) b |= DM_BTN_ZL; if (g_ax[5] > 0.5f) b |= DM_BTN_ZR;
    if (g_ax[6] < -0.5f) b |= DM_BTN_LEFT; if (g_ax[6] > 0.5f) b |= DM_BTN_RIGHT; if (g_ax[7] < -0.5f) b |= DM_BTN_UP; if (g_ax[7] > 0.5f) b |= DM_BTN_DOWN;
    dm_pad_set(b, axis(g_ax[0]), -axis(g_ax[1]), axis(g_ax[2]), -axis(g_ax[3]));   // Android's y grows downward, the Switch's upward
}
// ---- the on-screen gamepad (overlay_layout.h): a pointer that goes down on a control keeps it until it lifts; every other pointer is the game's touch screen ----
static _Atomic int g_overlay = 1;   // shown until a physical controller is used, again at the next touch (when the player has it on)
static _Atomic int g_pads;          // controllers attached (pad_watch): while there is one, nothing of the gamepad is shown, the menu button neither
static _Atomic int g_knob[2][2];    // the sticks' knob offsets, in 1/10000 of the stick radius
static _Atomic int64_t g_press_t[16], g_menu_t;   // when each overlay button (by DM_BTN_* bit) and the menu button went down, 0 = not held
static uint64_t g_touch_btn;        // the overlay buttons held, as of the last touch event
static int64_t mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
int dm_overlay_visible(void) { return atomic_load(&g_pads) ? 0 : atomic_load(&g_set_overlay) && atomic_load(&g_overlay) ? 1 : 2; }
void dm_overlay_menu_xy(float *x, float *y) {
    int mx = atomic_load(&g_mpos[0]), my = atomic_load(&g_mpos[1]);
    if (mx >= 0 && my >= 0) { *x = mx / 10000.0f; *y = my / 10000.0f; return; }
    *x = *y = 0; for (int i = 0; i < OV_N; i++) if (OV_CTLS[i].kind == OV_MENU) { *x = OV_CTLS[i].x; *y = OV_CTLS[i].y; }
}
int64_t dm_overlay_pressed(int ctl, uint64_t bit) {
    if (ctl < 0 || ctl >= OV_N) return 0;
    if (OV_CTLS[ctl].kind == OV_MENU) return atomic_load(&g_menu_t);
    if (OV_CTLS[ctl].kind == OV_BUTTON) bit = OV_CTLS[ctl].btn;
    return bit && bit < 1u << 16 ? atomic_load(&g_press_t[__builtin_ctzll(bit)]) : 0;
}
static int g_dpi;   // the display's density (AConfiguration), 0 = unknown
float dm_overlay_scale(void) {
    static int last_h = -1; static float s = 1;
    ANativeWindow *w = dm_front_window(); int h = w ? ANativeWindow_getHeight(w) : 0;
    if (h != last_h) { last_h = h; s = 1;
        if (h > 0 && g_dpi > 0 && g_dpi < 0xfffe) { float mm = 0.1f * (float)h / (float)g_dpi * 25.4f; s = OV_MIN_MM / mm; if (s < 1) s = 1; if (s > 1.45f) s = 1.45f; }   // (more and the face buttons touch on a 20:9 phone)
        LOG("[overlay] window height %d px, density %d dpi: controls x%.2f", h, g_dpi, s); }
    return s;
}
void dm_overlay_knob(int stick, float *dx, float *dy) { *dx = atomic_load(&g_knob[stick][0]) / 10000.0f; *dy = atomic_load(&g_knob[stick][1]) / 10000.0f; }
static int g_ptr_ctl[32];   // pointer id → control index + 1 (0 = the touch screen)
static int ctl_at(float x, float y, float W, float H) {   // the control under (x, y) in pixels, -1 if none
    int best = -1, vis = dm_overlay_visible(), shown = vis == 1; float bd = 1e9f;
    if (!vis) return -1;   // a controller is attached: every touch is the game's
    for (int i = 0; i < OV_N; i++) {
        const ov_ctl *c = &OV_CTLS[i]; if (!shown && c->kind != OV_MENU) continue;   // hidden: only the menu button answers
        float cx, cy; ov_xy(c, &cx, &cy);
        float r = ov_r(c), dx = (x - cx * W) / H, dy = (y - cy * H) / H, d = r * (1 + ov_sdf(c, dx / r, dy / r));   // a disc's: the distance from its centre
        float reach = c->kind == OV_BUTTON || c->kind == OV_MENU ? fmaxf(r * 1.25f, OV_REACH_MIN * dm_overlay_scale()) : r * 1.5f;
        if (d < reach && d / reach < bd) { bd = d / reach; best = i; }
    }
    return best;
}
// dragging the menu button: the pointer holding it, whether it has moved past OV_DRAG_SLOP, where it went down and the finger's offset from the button's centre
static struct { int id, moved; float x0, y0, gx, gy; } g_drag = {-1};
static void menu_grab(int id, float x, float y, float W, float H) {
    float mx, my; dm_overlay_menu_xy(&mx, &my);
    g_drag.id = id; g_drag.moved = 0; g_drag.x0 = x; g_drag.y0 = y; g_drag.gx = mx * W - x; g_drag.gy = my * H - y;
}
static void menu_drag(const ov_ctl *m, float x, float y, float W, float H) {   // the button follows the finger once it has moved, kept whole on the screen
    if (!g_drag.moved && hypotf(x - g_drag.x0, y - g_drag.y0) < OV_DRAG_SLOP * H) return;
    g_drag.moved = 1;
    float r = ov_r(m) * H, px = fminf(fmaxf(x + g_drag.gx, r), W - r), py = fminf(fmaxf(y + g_drag.gy, r), H - r);
    atomic_store(&g_mpos[0], (int)(px / W * 10000)); atomic_store(&g_mpos[1], (int)(py / H * 10000));
}

// ---- controllers attached: InputDevice's list, read once a second through JNI (a code-less NativeActivity has no InputManager listener). A device counts
// when it is a gamepad or a joystick and not virtual (the tablet's own keys, touch screen and pen are neither). While one is attached nothing of the gamepad
// is shown and every touch is the game's; its keys open the menu (Back, Mode, Menu: handle_event). When the last one goes, the gamepad comes back ----
static int jx(JNIEnv *E) { if (!(*E)->ExceptionCheck(E)) return 0; (*E)->ExceptionClear(E); return 1; }
static void *pad_watch(void *a) {
    JavaVM *vm = a; JNIEnv *E; pthread_setname_np(pthread_self(), "padwatch");
    if ((*vm)->AttachCurrentThread(vm, &E, NULL) != JNI_OK) { LOG("[RED] [input] cannot attach to the Java VM: attached controllers are not watched"); return NULL; }
    jclass cID = (*E)->FindClass(E, "android/view/InputDevice");
    jmethodID mIds = cID ? (*E)->GetStaticMethodID(E, cID, "getDeviceIds", "()[I") : NULL, mGet = cID ? (*E)->GetStaticMethodID(E, cID, "getDevice", "(I)Landroid/view/InputDevice;") : NULL;
    jmethodID mSrc = cID ? (*E)->GetMethodID(E, cID, "getSources", "()I") : NULL, mVirt = cID ? (*E)->GetMethodID(E, cID, "isVirtual", "()Z") : NULL;
    jmethodID mName = cID ? (*E)->GetMethodID(E, cID, "getName", "()Ljava/lang/String;") : NULL;
    if (jx(E) || !mIds || !mGet || !mSrc || !mVirt || !mName) { LOG("[RED] [input] InputDevice methods missing: attached controllers are not watched"); return NULL; }
    int prev[16], np = 0;
    for (;;) {
        int ids[16], n = 0;
        jintArray arr = (*E)->CallStaticObjectMethod(E, cID, mIds);
        if (!jx(E) && arr) {
            jsize len = (*E)->GetArrayLength(E, arr); jint *v = (*E)->GetIntArrayElements(E, arr, NULL);
            for (jsize i = 0; v && i < len && n < 16; i++) {
                jobject d = (*E)->CallStaticObjectMethod(E, cID, mGet, v[i]); if (jx(E) || !d) continue;
                jint src = (*E)->CallIntMethod(E, d, mSrc); jboolean virt = (*E)->CallBooleanMethod(E, d, mVirt);
                if (!jx(E) && !virt && ((src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD || (src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK)) {
                    ids[n++] = v[i]; int known = 0; for (int k = 0; k < np; k++) known |= prev[k] == v[i];
                    if (!known) {
                        jstring s = (*E)->CallObjectMethod(E, d, mName); const char *nm = !jx(E) && s ? (*E)->GetStringUTFChars(E, s, NULL) : NULL;
                        LOG("[input] controller attached: %s (device %d): the gamepad is hidden", nm ? nm : "?", v[i]);
                        if (nm) (*E)->ReleaseStringUTFChars(E, s, nm);
                        if (s) (*E)->DeleteLocalRef(E, s);
                    }
                }
                (*E)->DeleteLocalRef(E, d);
            }
            if (v) (*E)->ReleaseIntArrayElements(E, arr, v, JNI_ABORT);
            (*E)->DeleteLocalRef(E, arr);
        }
        for (int k = 0; k < np; k++) { int still = 0; for (int i = 0; i < n; i++) still |= ids[i] == prev[k]; if (!still) LOG("[input] controller detached (device %d)", prev[k]); }
        if (atomic_exchange(&g_pads, n) && !n) {   // the last one went: the gamepad again, and the device's motor for rumble
            atomic_store(&g_overlay, 1); dm_haptics_android_device(-1);
            LOG("[input] no controller attached: the gamepad is shown");
        }
        memcpy(prev, ids, sizeof ids); np = n;
        sleep(1);
    }
    return NULL;
}

// ---- the pause menu: the game is held while it is open; touches and a controller's keys go to it ----
static _Atomic int g_menu_hi = -1;   // the highlighted item (a controller's selection, or the row a finger is on), -1 none
static int g_menu_ptr = -1, g_menu_row = -2;   // the pointer pressing in the menu, and the row it went down on (-3 the menu button)
static _Atomic int g_tx_ver;   // the text entry's edit count: the panel is drawn again when it changes
int dm_menu_state(void) {
    if (atomic_load(&g_tx_on)) return 1 << 16 | (atomic_load(&g_tx_ver) & 0xffff);   // the text entry's panel (gfx_vk_present.c ov_text_panel)
    if (!atomic_load(&g_menu)) return -1;
    return atomic_load(&g_set_overlay) | atomic_load(&g_set_rumble) << 1 | (dm_opmode() != 0) << 2 | atomic_load(&g_set_fps) << 3 | (atomic_load(&g_menu_hi) + 1) << 4;
}
int dm_overlay_fps(void) { return atomic_load(&g_set_fps) && !atomic_load(&g_menu); }   // gfx_vk_present.c draws the counter (not while the menu holds the game)
static void menu_open(int hi) {
    if (!atomic_load(&g_running) || atomic_exchange(&g_menu, 1)) return;
    atomic_store(&g_menu_hi, hi); g_menu_ptr = -1;
    dm_input_set(0, 0, 0, 0, 0); dm_touch_set(NULL, 0); g_touch_btn = 0;   // nothing stays pressed in the game
    for (int i = 0; i < 16; i++) atomic_store(&g_press_t[i], 0);
    for (int s = 0; s < 2; s++) for (int a = 0; a < 2; a++) atomic_store(&g_knob[s][a], 0);
    if (g_pad_used) { g_btn = 0; memset(g_ax, 0, sizeof g_ax); pad_publish(); }
    for (int i = 0; i < 32; i++) g_ptr_ctl[i] = 0;
    dm_haptics_suspend(4, 1);   // (bits 1 and 2 are the iOS front end's reasons)
    hold_sync(); dm_overlay_redraw();
    LOG("[menu] opened: the game is paused");
}
static void menu_close(void) {
    if (!atomic_exchange(&g_menu, 0)) return;
    g_menu_ptr = -1; atomic_store(&g_menu_t, 0);
    hold_sync(); dm_haptics_suspend(4, 0); dm_overlay_redraw();
    LOG("[menu] closed: the game goes on (gamepad %s, rumble %s, frame rate %s, %s)", atomic_load(&g_set_overlay) ? "on" : "off", atomic_load(&g_set_rumble) ? "on" : "off",
        atomic_load(&g_set_fps) ? "shown" : "hidden", dm_opmode() ? "docked" : "handheld");
}
static void menu_do(int item) {
    if (item == OVM_OVERLAY) { atomic_store(&g_set_overlay, !atomic_load(&g_set_overlay)); atomic_store(&g_overlay, 1); settings_save(); }
    else if (item == OVM_RUMBLE) { int on = !atomic_load(&g_set_rumble); atomic_store(&g_set_rumble, on); dm_haptics_android_set(on); settings_save(); }
    else if (item == OVM_FPS) { atomic_store(&g_set_fps, !atomic_load(&g_set_fps)); settings_save(); }
    else if (item == OVM_MODE) { dm_set_opmode(dm_opmode() ? 0 : 1); settings_save(); }   // the game takes it when it goes on
    if (item >= 0 && item < OVM_N) dm_haptics_android_tap();
    if (item == OVM_RESUME) menu_close(); else dm_overlay_redraw();
}
static void menu_touch(AInputEvent *e, float W, float H) {   // a press on a row does it when it lifts there; a tap outside the panel or on the menu button closes
    int act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK, ai = AMotionEvent_getAction(e) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    int i = act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_UP ? 0 : ai, id = AMotionEvent_getPointerId(e, (size_t)i) & 31;
    float x = AMotionEvent_getX(e, (size_t)i), y = AMotionEvent_getY(e, (size_t)i);
    int c = ctl_at(x, y, W, H), row = c >= 0 && OV_CTLS[c].kind == OV_MENU ? -3 : ov_menu_row_at(x, y, W, H);
    if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        if (g_menu_ptr >= 0) return;
        g_menu_ptr = id; g_menu_row = row;
        if (row >= 0) { atomic_store(&g_menu_hi, row); dm_overlay_redraw(); }
    } else if (act == AMOTION_EVENT_ACTION_MOVE && g_menu_ptr >= 0 && g_menu_row >= 0) {   // the highlight follows the finger off its row and back
        for (size_t k = 0; k < AMotionEvent_getPointerCount(e); k++) if ((AMotionEvent_getPointerId(e, k) & 31) == g_menu_ptr) {
            int r = ov_menu_row_at(AMotionEvent_getX(e, k), AMotionEvent_getY(e, k), W, H), hi = r == g_menu_row ? r : -1;
            if (atomic_exchange(&g_menu_hi, hi) != hi) dm_overlay_redraw();
        }
    } else if ((act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_POINTER_UP) && id == g_menu_ptr) {
        int down = g_menu_row; g_menu_ptr = -1; atomic_store(&g_menu_hi, -1);
        if (row == down && row >= 0) menu_do(row);
        else if (row == down && (row == -2 || row == -3)) { dm_haptics_android_tap(); menu_close(); }
        else dm_overlay_redraw();
    } else if (act == AMOTION_EVENT_ACTION_CANCEL) { g_menu_ptr = -1; atomic_store(&g_menu_hi, -1); dm_overlay_redraw(); }
}
static void menu_key(uint64_t bit) {   // a controller key pressed while the menu is open
    int hi = atomic_load(&g_menu_hi);
    if (bit == DM_BTN_UP || bit == DM_BTN_DOWN) { hi = hi < 0 ? 0 : (hi + (bit == DM_BTN_UP ? OVM_N - 1 : 1)) % OVM_N; atomic_store(&g_menu_hi, hi); dm_overlay_redraw(); }
    else if (bit == DM_BTN_A) menu_do(hi < 0 ? OVM_RESUME : hi);
    else if (bit == DM_BTN_B || bit == DM_BTN_PLUS) menu_close();
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
    if (atomic_load(&g_menu)) { menu_touch(e, W, H); return; }
    int act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK, ai = AMotionEvent_getAction(e) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    size_t pc = AMotionEvent_getPointerCount(e);
    if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        int i = act == AMOTION_EVENT_ACTION_DOWN ? 0 : ai, id = AMotionEvent_getPointerId(e, (size_t)i) & 31;
        int c = ctl_at(AMotionEvent_getX(e, (size_t)i), AMotionEvent_getY(e, (size_t)i), W, H);
        g_ptr_ctl[id] = c + 1;
        if (c >= 0 && OV_CTLS[c].kind == OV_MENU) { atomic_store(&g_menu_t, mono_ns()); dm_haptics_android_tap(); menu_grab(id, AMotionEvent_getX(e, (size_t)i), AMotionEvent_getY(e, (size_t)i), W, H); }
        if (!atomic_load(&g_overlay) && !atomic_load(&g_pads)) { atomic_store(&g_overlay, 1); dm_haptics_android_device(-1); }   // a touch brings the gamepad back (not over an attached controller)
    }
    uint64_t btn = 0; int32_t lx = 0, ly = 0, rx = 0, ry = 0; float kn[2][2] = {{0, 0}, {0, 0}}; int open = 0;
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
                if (id == g_drag.id && g_drag.moved) { settings_save(); LOG("[overlay] menu button moved to %.3f, %.3f", atomic_load(&g_mpos[0]) / 10000.0, atomic_load(&g_mpos[1]) / 10000.0); }
                else open = act != AMOTION_EVENT_ACTION_CANCEL;
                if (id == g_drag.id) g_drag.id = -1;
            }
            g_ptr_ctl[id] = 0; continue;
        }
        if (c < 0) { if (n < 16) { t[n].id = (uint32_t)id & 15; t[n].x = (x / W - pic[0]) / pic[2]; t[n].y = (y / H - pic[1]) / pic[3]; n++; } continue; }   // relative to the game's picture
        const ov_ctl *k = &OV_CTLS[c];
        if (k->kind == OV_MENU) { if (id == g_drag.id) menu_drag(k, x, y, W, H); continue; }
        float dx = (x - k->x * W) / (ov_r(k) * H), dy = (y - k->y * H) / (ov_r(k) * H), m = sqrtf(dx * dx + dy * dy);
        if (k->kind == OV_BUTTON) btn |= k->btn;
        else if (k->kind == OV_DPAD) {
            if (m > 0.25f) { float a = atan2f(dy, dx); int s = (int)lroundf(a / (3.14159265f / 4)) & 7;   // 8 directions, 0 = right, clockwise (y grows downward)
                static const uint64_t dir[8] = {DM_BTN_RIGHT, DM_BTN_RIGHT | DM_BTN_DOWN, DM_BTN_DOWN, DM_BTN_DOWN | DM_BTN_LEFT, DM_BTN_LEFT, DM_BTN_LEFT | DM_BTN_UP, DM_BTN_UP, DM_BTN_UP | DM_BTN_RIGHT};
                btn |= dir[s]; }
        } else {
            if (m > 1) { dx /= m; dy /= m; }
            kn[k->kind == OV_STICK_R][0] = dx; kn[k->kind == OV_STICK_R][1] = dy;   // the knob follows the finger (to the ring)
            int32_t sx = (int32_t)(dx * DM_STICK_MAX), sy = (int32_t)(-dy * DM_STICK_MAX);
            if (k->kind == OV_STICK_L) { lx = sx; ly = sy; } else { rx = sx; ry = sy; }
        }
    }
    uint64_t down = btn & ~g_touch_btn, up = g_touch_btn & ~btn; int64_t now = mono_ns(); g_touch_btn = btn;
    for (int b = 0; b < 16; b++) { if (down >> b & 1) atomic_store(&g_press_t[b], now); else if (up >> b & 1) atomic_store(&g_press_t[b], 0); }
    if (down) dm_haptics_android_tap();   // a tick for each button (or d-pad direction) the finger reaches
    for (int s = 0; s < 2; s++) for (int a = 0; a < 2; a++) atomic_store(&g_knob[s][a], (int)(kn[s][a] * 10000));
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
        uint64_t bit = key_bit(k);
        if (!bit) {   // the rest: the system's. A controller's key nothing maps is logged once, so a new controller's menu key can be found
            static uint8_t seen[64];
            if ((src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD && down && k >= 0 && k < 512 && !(seen[k >> 3] >> (k & 7) & 1)) {
                seen[k >> 3] |= (uint8_t)(1 << (k & 7)); LOG("[input] controller key %d (device %d): not mapped", k, AInputEvent_getDeviceId(e)); }
            return 0;
        }
        atomic_store(&g_overlay, 0); g_pad_used = 1; dm_haptics_android_device(AInputEvent_getDeviceId(e));   // a controller is in use: the gamepad steps aside (until a touch)
        if (atomic_load(&g_menu)) { if (down && AKeyEvent_getRepeatCount(e) == 0) menu_key(bit); return 1; }
        if (down) g_btn |= bit; else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) g_btn &= ~bit;
        dm_pad_players(1); pad_publish(); return 1;
    }
    if (atomic_load(&g_tx_on) && ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK || (src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD)) return 1;   // sticks: not for the text entry
    if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK || (src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD) {
        g_pad_used = 1; dm_haptics_android_device(AInputEvent_getDeviceId(e));
        if (atomic_load(&g_menu)) {   // the hat moves the selection
            static float hy; float y = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
            if (y < -0.5f && hy >= -0.5f) menu_key(DM_BTN_UP); else if (y > 0.5f && hy <= 0.5f) menu_key(DM_BTN_DOWN);
            hy = y; return 1;
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
static void *input_thread(void *a) {
    (void)a; g_ilooper = ALooper_prepare(0);
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
    act->callbacks->onPause = on_pause; act->callbacks->onResume = on_resume; act->callbacks->onDestroy = on_destroy;
    ANativeActivity_setWindowFlags(act, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    static int started; if (started++) return;   // a second onCreate (configuration change) keeps the running game
    snprintf(g_root, sizeof g_root, "%s", act->externalDataPath ? act->externalDataPath : act->internalDataPath);
    { char ready[700]; snprintf(ready, sizeof ready, "%s/.nx2a-imported/.ready", g_root);
      if (!access(ready, R_OK)) { size_t n = strlen(g_root); snprintf(g_root + n, sizeof g_root - n, "/.nx2a-imported"); } }
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
