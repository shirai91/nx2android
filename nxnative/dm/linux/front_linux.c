// Linux port front end (dm/host/front.h): an SDL2 window with a Vulkan surface, the gamepad and the keyboard.
// The guest runs on its own thread; the main thread owns the window and pumps its events. DM_HEADLESS=1 (or no display) runs the guest on the calling
// thread with no window. The Vulkan present code (android/gfx_vk_present.c) takes the window through dm_front_window(), dm_front_create_surface() and
// ANativeWindow_getWidth/Height() below; the Vulkan instance takes its extensions from dm_front_instance_exts().
#include "front.h"
#include "dm_input.h"
#include <android/native_window.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL.h>
#include <SDL_vulkan.h>

static SDL_Window *g_win;
static atomic_int g_quit;
static void (*g_guest)(void);

// ---- what the Vulkan backend asks of the window ----
ANativeWindow *dm_front_window(void) { return (ANativeWindow *)g_win; }
int ANativeWindow_getWidth(ANativeWindow *w) { (void)w; int ww = 0, hh = 0; if (g_win) SDL_Vulkan_GetDrawableSize(g_win, &ww, &hh); return ww; }
int ANativeWindow_getHeight(ANativeWindow *w) { (void)w; int ww = 0, hh = 0; if (g_win) SDL_Vulkan_GetDrawableSize(g_win, &ww, &hh); return hh; }
int dm_front_create_surface(void *instance, void *surface_out) {   // 0 = made
    return g_win && SDL_Vulkan_CreateSurface(g_win, (VkInstance)instance, (VkSurfaceKHR *)surface_out) ? 0 : -1;
}
int dm_front_instance_exts(const char **names, int max) {   // the instance extensions the window needs; 0 without a window
    unsigned n = 0;
    if (!g_win || !SDL_Vulkan_GetInstanceExtensions(g_win, &n, NULL) || (int)n > max) return 0;
    return SDL_Vulkan_GetInstanceExtensions(g_win, &n, names) ? (int)n : 0;
}

// ---- the performance hooks the Android build gives to ADPF: nothing on Linux (the swapchain paces the presents) ----
void dm_perf_frame(double period) { (void)period; }
void dm_perf_window(ANativeWindow *w, double fps) { (void)w; (void)fps; }
void dm_perf_frame_cost(int64_t replay_ns, int64_t gpu_ns) { (void)replay_ns; (void)gpu_ns; }

// ---- the rest of front.h ----
int front_screen_max_fps(void) { SDL_DisplayMode m; return g_win && !SDL_GetCurrentDisplayMode(0, &m) ? m.refresh_rate : 0; }
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap; return -1;   // (to do: an on-screen keyboard)
}
__attribute__((weak)) int mtl_tex_gpu_owned(uint64_t serial) { (void)serial; return 0; }

// ---- input: the first game controller and the keyboard, as player 1 ----
static SDL_GameController *g_pad;
static uint64_t g_kbd;
static void pad_open(void) {
    if (g_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i) && (g_pad = SDL_GameControllerOpen(i))) { fprintf(stderr, "[dm] controller: %s\n", SDL_GameControllerName(g_pad)); return; }
}
static int axis(int16_t v) { return v > -3000 && v < 3000 ? 0 : v; }
static void push_input(void) {
    uint64_t b = g_kbd; int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    if (g_pad) {
#define BTN(sdl, dm) if (SDL_GameControllerGetButton(g_pad, sdl)) b |= (dm)
        BTN(SDL_CONTROLLER_BUTTON_B, DM_BTN_A); BTN(SDL_CONTROLLER_BUTTON_A, DM_BTN_B);   // by position, as the Switch's layout: the east button is A
        BTN(SDL_CONTROLLER_BUTTON_Y, DM_BTN_X); BTN(SDL_CONTROLLER_BUTTON_X, DM_BTN_Y);
        BTN(SDL_CONTROLLER_BUTTON_LEFTSTICK, DM_BTN_LS); BTN(SDL_CONTROLLER_BUTTON_RIGHTSTICK, DM_BTN_RS);
        BTN(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, DM_BTN_L); BTN(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, DM_BTN_R);
        BTN(SDL_CONTROLLER_BUTTON_START, DM_BTN_PLUS); BTN(SDL_CONTROLLER_BUTTON_BACK, DM_BTN_MINUS);
        BTN(SDL_CONTROLLER_BUTTON_DPAD_LEFT, DM_BTN_LEFT); BTN(SDL_CONTROLLER_BUTTON_DPAD_UP, DM_BTN_UP);
        BTN(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, DM_BTN_RIGHT); BTN(SDL_CONTROLLER_BUTTON_DPAD_DOWN, DM_BTN_DOWN);
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) b |= DM_BTN_ZL;
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) b |= DM_BTN_ZR;
        lx = axis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX)); ly = -axis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY));
        rx = axis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX)); ry = -axis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY));   // the Switch's y grows upward
#undef BTN
    }
    dm_pad_set(b, lx, ly, rx, ry);
}
static uint64_t key_button(SDL_Keycode k) {   // a keyboard for bring-up: arrows; X/Z/S/A = A/B/X/Y; Return = Plus; Right Shift = Minus; Q/E = L/R; 1/2 = ZL/ZR
    switch (k) {
    case SDLK_LEFT: return DM_BTN_LEFT; case SDLK_UP: return DM_BTN_UP; case SDLK_RIGHT: return DM_BTN_RIGHT; case SDLK_DOWN: return DM_BTN_DOWN;
    case SDLK_x: return DM_BTN_A; case SDLK_z: return DM_BTN_B; case SDLK_s: return DM_BTN_X; case SDLK_a: return DM_BTN_Y;
    case SDLK_RETURN: return DM_BTN_PLUS; case SDLK_RSHIFT: return DM_BTN_MINUS;
    case SDLK_q: return DM_BTN_L; case SDLK_e: return DM_BTN_R; case SDLK_1: return DM_BTN_ZL; case SDLK_2: return DM_BTN_ZR;
    default: return 0;
    }
}

static void *guest_thread(void *unused) { (void)unused; g_guest(); atomic_store(&g_quit, 1); return NULL; }

void front_run(void (*guest)(void)) {
    const char *hl = getenv("DM_HEADLESS");
    if ((hl && *hl && *hl != '0') || SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        if (!(hl && *hl)) fprintf(stderr, "[dm] SDL: %s; running without a window\n", SDL_GetError());
        guest(); return;
    }
    g_guest = guest;
    int fs = getenv("DM_FULLSCREEN") && atoi(getenv("DM_FULLSCREEN")), w = 1280, h = 960;
    if (getenv("DM_WINDOW_SIZE")) sscanf(getenv("DM_WINDOW_SIZE"), "%dx%d", &w, &h);
    g_win = SDL_CreateWindow("nx2android", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | (fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!g_win) { fprintf(stderr, "[dm] SDL_CreateWindow: %s; running without a window\n", SDL_GetError()); guest(); return; }
    SDL_ShowCursor(SDL_DISABLE);
    pad_open();
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 16 << 20);   // the game's main thread: a 16 MB stack, as on Android
    pthread_t t; if (pthread_create(&t, &at, guest_thread, NULL)) { fprintf(stderr, "[dm] cannot start the guest thread\n"); guest(); return; }
    while (!atomic_load(&g_quit)) {
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 8)) {
            switch (e.type) {
            case SDL_QUIT: raise(SIGTERM); break;   // the host's graceful exit (hle_lifecycle.c)
            case SDL_CONTROLLERDEVICEADDED: pad_open(); break;
            case SDL_CONTROLLERDEVICEREMOVED: if (g_pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) { SDL_GameControllerClose(g_pad); g_pad = NULL; pad_open(); } break;
            case SDL_KEYDOWN: if (!e.key.repeat) g_kbd |= key_button(e.key.keysym.sym); if (e.key.keysym.sym == SDLK_ESCAPE) raise(SIGTERM); break;
            case SDL_KEYUP: g_kbd &= ~key_button(e.key.keysym.sym); break;
            }
        }
        push_input();
    }
    pthread_join(t, NULL);
    SDL_DestroyWindow(g_win); g_win = NULL; SDL_Quit();
}

// ---- the on-screen gamepad's theme (android/overlay_theme.c): there is no on-screen gamepad on Linux ----
#include "overlay_layout.h"
const ov_theme *dm_overlay_theme(int layout) { (void)layout; return NULL; }
void dm_overlay_theme_done(const ov_theme *t) { (void)t; }
