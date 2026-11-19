// The frame-pacing policy: one place that decides, from the game profile (game.tsv fps_target, fps_ceiling, fps_hold, present_interval) and the switches over it
// (nxnative/dm/settings.tsv), how a game's frames are paced. The mechanisms stay where they run: the frame step (nvn_fstep.h, loader.c), the 60 fps memory
// writes (hle_fpslock.c), the present interval (hle_nvn.c), presents and the window's rate vote (gfx_vk_present.c), the adaptive governor (perf_android.c).
// They ask this module what to do, and dm_pacing_log() writes the whole policy as one log line. docs/ARCHITECTURE.md, Frame pacing.
#pragma once

// The frame-rate target: 0 (the game's own limit), 30 or 60. The profile's fps_target, DM_FPS_TARGET over it; another value is logged and gives 0.
// *src (when not NULL) names where it came from, for logs.
int dm_pacing_target(const char **src);
// The 30 fps ceiling the profile keeps (fps_ceiling), DM_FPS_CEILING over it (porter --fps60 writes 0). 30 or 0
int dm_pacing_ceiling(void);
// Whether late frames may raise the frame step's hold above the cap's vsyncs (fps_hold; DM_FPS_HOLD=off turns it off)
int dm_pacing_hold(const char **src);
// Whether presents follow the display (present_interval display; DM_PRESENT_INTERVAL=display) instead of the game's present interval
int dm_pacing_display(const char **src);
// The adaptive governor (Android): on unless DM_ADAPTIVE_FPS=0. *explicit_set is 1 when the switch is set at all (a player's or profile's choice)
int dm_pacing_adaptive(int *explicit_set);
// The governor pinned at a rate: DM_FPS_PIN=60, 40 or 30; 0 when not pinned (any other value)
int dm_pacing_pin(void);
// The percentile of frame work the governor judges for a step up (DM_ADAPT_UP_PCT, 1..99, default 90)
int dm_pacing_up_pct(void);
// Whether the window asks the display for the game's own rate (DM_VK_WINDOW_GAME_RATE=1)
int dm_pacing_window_game_rate(void);
// GPU turbo: 0 off, 1 on, 2 auto (DM_GPU_TURBO=1 / auto)
int dm_pacing_turbo(void);
// One log line with the whole policy (at start-up, after the game profile is known)
void dm_pacing_log(void);
