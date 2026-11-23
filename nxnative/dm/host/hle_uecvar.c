// Unreal console variables the profile raises in game memory (its ue_cvars.tsv, written into game_profile.h as DM_GAME_UE_CVARS). The host row exists for the texture streaming
// pool: a game's Switch device profile can size r.Streaming.PoolSize below the value its Windows profile uses (DRAGON QUEST VII Reimagined: 200 MB against 1000), and the streamer
// then drops the top mips of the textures in view. Adapted from seedds/nxnative dm/host/hle_uecvar.c (881c6184, its issue #589) under the MIT licence. A TAutoConsoleVariable is a
// global in main's data whose +0x10 holds a pointer to the value, the game thread's copy and then the render thread's; the streaming manager reads its copy again when it changes.
// Each row runs only on the main build it was found in (found by the registration scan of upstream nxnative's dm/tools/ue_cvar.py, which this repository does not carry). Called
// on every present (hle_nvn.c), like hle_fpslock.c's frame limits: the game sets its values again.
#include "hle.h"
#include <mach/mach.h>

static int rd(uint64_t a, void *out, size_t n) { vm_size_t got = 0; return a >= 0x1000 && vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n; }
// guest pointers can carry a top-byte tag (Android's MTE; the CPU ignores it), reads through the compat layer here take it off first
static uint64_t untag(uint64_t p) { return p & 0x00FFFFFFFFFFFFFFull; }
enum { INT32 };
static const struct { const char *build, *what; uint64_t obj_off; uint32_t field; uint8_t kind, copies; uint32_t value; } k_cvars[] = {
#ifdef DM_GAME_UE_CVARS
    DM_GAME_UE_CVARS
#else
    { NULL }
#endif
};
#ifdef DM_GAME_UE_CVARS
#define NCV DM_GAME_UE_CVARS_N
#else
#define NCV 0
#endif

void dm_ue_cvar_tick(void) {
    static int inited, nrow, rows[NCV > 0 ? NCV : 1], nlog[NCV > 0 ? NCV : 1]; static uint8_t *base;
    if (!inited) {
        inited = 1;
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
            base = g_mod[i].base;
            for (int k = 0; k < NCV; k++) if (!strcmp(g_mod[i].build_id, k_cvars[k].build) && nrow < (int)(sizeof rows / sizeof rows[0])) {
                rows[nrow++] = k;
                LOG("main: Unreal %s at main+%#llx, raised to %u (game profile ue_cvars.tsv)", k_cvars[k].what, (unsigned long long)k_cvars[k].obj_off, k_cvars[k].value);
            }
        }
        return;
    }
    for (int r = 0; r < nrow; r++) {
        int k = rows[r]; uint64_t val = 0;
        if (!rd((uint64_t)(uintptr_t)base + k_cvars[k].obj_off + k_cvars[k].field, &val, sizeof val) || !val) continue;   // not created yet
        val = untag(val);
        for (int c = 0; c < k_cvars[k].copies; c++) {
            uint64_t a = val + 4 * (uint64_t)c; int32_t cur;
            if (!rd(a, &cur, sizeof cur)) break;
            if (cur <= 0 || cur >= (int32_t)k_cvars[k].value) continue;
            int32_t *p = (int32_t *)(uintptr_t)a; dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, (int32_t)k_cvars[k].value, __ATOMIC_RELAXED);
            if (nlog[k] < 8) { nlog[k]++; LOG("main: Unreal %s (%s thread) %d → %u (game profile)", k_cvars[k].what, c ? "render" : "game", cur, k_cvars[k].value); }
        }
    }
}