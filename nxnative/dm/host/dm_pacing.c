// The frame-pacing policy (dm_pacing.h): the profile's pacing keys and the switches over them, read in one place. No state: each call reads the switches
// again (cheap on Android: dm_settings.h caches getenv), so the live ones (DM_FPS_PIN, DM_VK_WINDOW_GAME_RATE, DM_GPU_TURBO) stay live; the callers that
// read a value once keep their own copy, as before.
#include "dm_pacing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_profile.h"   // DM_GAME_FPS_TARGET, DM_GAME_FPS_CEILING, DM_GAME_FPS_HOLD, DM_GAME_PRESENT_DISPLAY

#define PLOG(...) do { fprintf(stderr, "[dm] " __VA_ARGS__); fputc('\n', stderr); } while (0)

int dm_pacing_target(const char **src) {
    const char *e = getenv("DM_FPS_TARGET"); int t = e ? atoi(e) : DM_GAME_FPS_TARGET;
    if (src) *src = e ? "DM_FPS_TARGET" : "game profile fps_target";
    if (t != 0 && t != 30 && t != 60) {
        static int said; if (!said++) PLOG("[RED] DM_FPS_TARGET=%s: must be 0, 30 or 60; the game's own frame limit is used", e ? e : "(profile)");
        t = 0;
    }
    return t;
}

int dm_pacing_ceiling(void) { const char *e = getenv("DM_FPS_CEILING"); return e ? atoi(e) : DM_GAME_FPS_CEILING; }

int dm_pacing_hold(const char **src) {
    const char *e = getenv("DM_FPS_HOLD");
    if (src) *src = e ? "DM_FPS_HOLD" : "game profile fps_hold";
    return e ? strcmp(e, "off") != 0 : DM_GAME_FPS_HOLD;
}

int dm_pacing_display(const char **src) {
    const char *m = getenv("DM_PRESENT_INTERVAL");
    if (src) *src = m ? "DM_PRESENT_INTERVAL" : "game profile present_interval";
    return m ? !strcmp(m, "display") : DM_GAME_PRESENT_DISPLAY;
}

int dm_pacing_adaptive(int *explicit_set) {
    const char *e = getenv("DM_ADAPTIVE_FPS");
    if (explicit_set) *explicit_set = e && *e;
    return !(e && !atoi(e));
}

int dm_pacing_pin(void) { const char *e = getenv("DM_FPS_PIN"); int v = e ? atoi(e) : 0; return v == 60 || v == 40 || v == 30 ? v : 0; }

int dm_pacing_up_pct(void) { const char *e = getenv("DM_ADAPT_UP_PCT"); return e && atoi(e) > 0 && atoi(e) < 100 ? atoi(e) : 90; }

int dm_pacing_window_game_rate(void) { const char *e = getenv("DM_VK_WINDOW_GAME_RATE"); return e && atoi(e) != 0; }

int dm_pacing_turbo(void) { const char *e = getenv("DM_GPU_TURBO"); return e && !strcmp(e, "auto") ? 2 : e && !strcmp(e, "need") ? 3 : e && atoi(e) > 0; }

void dm_pacing_log(void) {
    const char *ts, *hs, *ds; int t = dm_pacing_target(&ts), h = dm_pacing_hold(&hs), d = dm_pacing_display(&ds), ex, a = dm_pacing_adaptive(&ex), pin = dm_pacing_pin();
    static const char *turbo[] = {"off", "on", "auto", "when needed"};
    PLOG("pacing: target %s (%s), ceiling %d, hold %s (%s), presents %s (%s), governor %s%s, pin %d, step-up p%d, window game rate %s, GPU turbo %s",
         t ? (t == 30 ? "30 fps" : "60 fps") : "the game's own", ts, dm_pacing_ceiling(), h ? "on" : "off", hs, d ? "on the display's ticks" : "at the game's interval", ds,
         a ? "on" : "off", ex ? " (set)" : "", pin, dm_pacing_up_pct(), dm_pacing_window_game_rate() ? "on" : "off", turbo[dm_pacing_turbo()]);
}
