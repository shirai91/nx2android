// Engine frame limits kept in game memory (FPSLocker's evaluate_write entries): some engines cap their own frame rate with a value the host can raise. The target is the profile's
// fps_target (DM_FPS_TARGET overrides; 0 = leave the game's own limit), the same key that drives Tears of the Kingdom's frame step (loader.c install_fps_state). Called on every present
// (hle_nvn.c); the game sets the values after boot and may set them again, so each is written whenever it differs. Fixed addresses: each row runs only on the main build it was found in (AGENTS.md).
// A port can also bring rows of its own from a FPSLocker config of its main build (DM_FPS_LOCKS, "Imported rows" below)
#include "hle.h"
#include <mach/mach.h>

// What a field holds: the frame rate as a float, vsyncs per frame (60 / frame rate) as an int32, or the frame time in milliseconds or seconds as a float. Copies: consecutive copies of the value
// (Unreal keeps one for the game thread, one for the render thread). A row with no pointer (ptr_off 0) has its field at main + field: a constant in main's read-only data, which the loader
// leaves writable (the image's data block on iOS too, loader.c map_image).
// At 60 a frame time gets MS_MARGIN less than the vsync: such a limiter counts from the frame's start, and the work after it (the GPU fence wait, the next frame's start) adds to the
// period, so frames at exactly 16.667 ms came 0.11 ms late each on the Mac (59.6 fps, every 150th frame shown two vsyncs). Slightly shorter, the host's present pacing sets the pace
// (the acquire waits): Luigi's Mansion 3's display interval p99 18.69 → 16.67 ms, 60.0 fps. Not at 30: with present_interval display the presents follow every vsync and hold no
// frame for two, so a shorter time showed some frames for one (30.4 fps); 30 writes the game's own 33.333 ms
enum { FPS_F32, VSYNCS_I32, MS_F32, SEC_F32 };
#define MS_MARGIN 0.5f
static float ms_want(int fps) { return 1000.0f / (float)fps - (fps == 60 ? MS_MARGIN : 0); }
static const struct { const char *build, *what; uint64_t ptr_off; uint32_t field; uint8_t kind, copies; uint32_t ptr2; } k_locks[] = {   // ptr2: a second pointer at that object + ptr2 (an IL2CPP class's static fields), 0 = none
    // Monster Hunter Rise 16.0.2: RE Engine's frame limit, a float at [main+0x1295b3f0]+0x434 (the pointer is a global in main's bss). The frame loop (main+0x6b106c8) sleeps until
    // [+0x430] / [+0x434] sixtieths of a second have passed ([+0x430] = 60) and steps the game by the time that really passed (+0x41c), so a higher limit runs the game faster in frames
    // at the same speed. The game sets 30 after boot; it also asks for present interval 2, which the profile's present_interval display ignores.
    // From masagrator's FPSLocker config for this build (FPSLocker-Warehouse, patches/0100B04011742000/C9A3DD7702075ECD.yaml)
    { "c9a3dd7702075ecd90101f4555389582", "RE Engine frame limit", 0x1295b3f0, 0x434, FPS_F32, 1 },
    // Octopath Traveler 0 1.0.5: Unreal Engine 5's console variables t.MaxFPS (the game thread's frame limit: it sleeps until 1 / t.MaxFPS has passed and steps the game by the time that
    // really passed) and rhi.SyncInterval (vsyncs per present: the RHI's present interval). Each is a TAutoConsoleVariable in main's bss (vtable, the console object, then at +0x10 the pointer to
    // its value: the game thread's copy, then the render thread's). The game's config sets t.MaxFPS 30 ([ConsoleVariables] of DefaultEngine.ini; FrameRateLimit=30 of SwitchGameUserSettings.ini
    // sets it again) and rhi.SyncInterval 2 (SwitchDeviceProfiles.ini), so the game asks for present interval 2. Found from the names' references in the static constructors that register them
    // (main+0x2ca6524, main+0x1026e54; RegisterConsoleVariable, then AsVariableFloat / AsVariableInt stored at +0x10)
    { "9fe60a212cff9b9a3656ab63d4f882e5", "Unreal t.MaxFPS", 0x7c738d0 + 0x10, 0, FPS_F32, 2 },
    { "9fe60a212cff9b9a3656ab63d4f882e5", "Unreal rhi.SyncInterval", 0x7b32690 + 0x10, 0, VSYNCS_I32, 2 },
    // DRAGON QUEST XI S 1.0.4: Unreal Engine 4.18's t.MaxFPS and nvn.SyncInterval (the Switch RHI's present interval), the same TAutoConsoleVariable layout. The game's config sets
    // FrameRateLimit=30 (DefaultGameUserSettings.ini) and nvn.SyncInterval=2 (JackGame/Config/Switch/SwitchEngine.ini). Found from the names' references (UTF-32, as the engine's TCHAR on
    // the Switch) in the static constructors: main+0x1e7f98 (TAutoConsoleVariable<float> constructor main+0x6ee0e0, AsVariableFloat stored at +0x10) and main+0x21c00c
    { "1719aabfa5eae42b132b65770b871288", "Unreal t.MaxFPS", 0x8501c38 + 0x10, 0, FPS_F32, 2 },
    { "1719aabfa5eae42b132b65770b871288", "Unreal nvn.SyncInterval", 0x85270b8 + 0x10, 0, VSYNCS_I32, 2 },
    // DRAGON QUEST VII Reimagined 1.2.0: Unreal Engine 4.27's t.MaxFPS and rhi.SyncInterval, the same layout (names in UTF-16, the engine's TCHAR in this build). The game's config sets
    // FrameRateLimit=30 (DefaultGameUserSettings.ini, SwitchGameUserSettings.ini) and rhi.syncinterval=2 (SwitchDeviceProfiles.ini). Registered at main+0x4a61b40 (float, AsVariableFloat
    // stored at +0x10) and main+0x36164f4 (int, AsVariableInt at +0x10)
    { "f7ec03e4cb9de3bd8319d4e899d2e9cc", "Unreal t.MaxFPS", 0x7f41908 + 0x10, 0, FPS_F32, 2 },
    { "f7ec03e4cb9de3bd8319d4e899d2e9cc", "Unreal rhi.SyncInterval", 0x7dedf38 + 0x10, 0, VSYNCS_I32, 2 },
    // FINAL FANTASY RESONANCE 1.0.1: Unreal Engine 5.6.1's t.MaxFPS and rhi.SyncInterval, the same layout (names in UTF-16). The engine's Switch device profile sets rhi.syncinterval=2
    // (BaseSwitchDeviceProfiles.ini). Registered at main+0x33a4dec (float, IConsoleManager slot 2; AsVariableFloat stored at +0x10) and main+0x149e0bc (int, slot 1; AsVariableInt at +0x10)
    { "84e182f72ee1da4f477804bd05b54c15", "Unreal t.MaxFPS", 0x8c29af0 + 0x10, 0, FPS_F32, 2 },
    { "84e182f72ee1da4f477804bd05b54c15", "Unreal rhi.SyncInterval", 0x8adca10 + 0x10, 0, VSYNCS_I32, 2 },
    // Luigi's Mansion 3 1.4.0: Next Level Games' frame limiter. At the end of each frame the main thread calls the frame callbacks (main+0xb7c308) with the milliseconds since the frame
    // started; the limiter (main+0x7a3e70) runs jobs or sleeps until 33.333 ms have passed, the float at main+0x13c94dc (read only there, at main+0x7a3ea8 and main+0x7a3f18), and the game
    // steps by the time that really passed. The game asks for present interval 2 too, which the profile's present_interval display ignores; both are needed
    { "79e5950ffa85acf63e28c9aec051eac2", "NLG frame limiter target", 0, 0x13c94dc, MS_F32, 1 },
    // Graveyard Keeper 2 1.0.4: Unity 6's vsync count and frame time, globals in main's data. Its vsync-count setter (main+0xd47310) stores the count (at least 1) at main+0x7d869d8 and
    // count / 60 seconds at main+0x7d869e4, which the frame pacing (main+0xd587f4) waits for 0.9 of after the last vsync; the game sets 2. Found from masagrator's FPSLocker config for
    // 1.0.2 (FPSLocker-Warehouse, patches/0100005027A18000/D8AEEEB4E7F1AB63.yaml: the same two fields, 1 / 62 s at 60), whose reference bytes occur once in this build's data
    { "1d69f09fc684a7d1cbd778b90a5df1e5", "Unity vsync count", 0, 0x7d869d8, VSYNCS_I32, 1 },
    { "1d69f09fc684a7d1cbd778b90a5df1e5", "Unity frame time", 0, 0x7d869e4, SEC_F32, 1 },
    // Advance Wars 1+2: Re-Boot Camp 1.0.0: Unity 2020.2's OnDemandRendering.renderFrameInterval, an IL2CPP static int: main+0x481b2f8 holds the class (Il2CppClass), whose +0xb8 points to its
    // static fields, the interval first. The game sets 1 in the menus and 2 in battle (Unity then renders every second frame: 30 fps, the frame's work 33 ms with the CPU and GPU idle).
    // From masagrator's FPSLocker config for this build (FPSLocker-Warehouse, patches/0100300012F2A000/320A17744AEFD67F.yaml, which writes -1); 1 renders every frame as well.
    // The host's log of that config on the Y700 read 1 at the title and 2 in the first training battle
    { "320a17744aefd67fec5738ca0c629795", "Unity render frame interval", 0x481b2f8, 0, VSYNCS_I32, 1, 0xb8 },
};
#define NLOCKS ((int)(sizeof k_locks / sizeof k_locks[0]))

static int rd(uint64_t a, void *out, size_t n) { vm_size_t got = 0; return a >= 0x1000 && vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n; }

// Imported rows (DM_FPS_LOCKS: fpslock.tsv in an Android port's data pack, written by porter/fpslocker.py): the memory writes of the community's FPSLocker config of this main
// build (masagrator's FPSLocker-Warehouse, its ALL_FPS entries), whose expressions the porter evaluated for 30 and 60. A row: the value type, main offsets (all but the last name
// a pointer), the values at 30 and at 60 (consecutive copies), and a compare that must hold for the write (type, offsets, operator, value; "-" = always). As FPSLocker does, every
// row is written on every present whatever the field holds (when it differs: no plausibility window, the configs are made for that); a pointer that is 0 or unreadable skips the
// row this time. The rows above win for their builds
enum { T_I8, T_I16, T_I32, T_I64, T_U8, T_U16, T_U32, T_U64, T_F32, T_F64, T_N };
static const char *const k_tname[T_N] = {"int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "float", "double"};
static const uint8_t k_tsize[T_N] = {1, 2, 4, 8, 1, 2, 4, 8, 4, 8};
enum { IMP_MAX = 64, IMP_CHAIN = 8, IMP_COPIES = 16 };
typedef struct { int n; int64_t off[IMP_CHAIN]; } imp_chain;
static struct { char what[32]; uint8_t type, ncopy, ctype, cop; imp_chain a, c; uint64_t v[2][IMP_COPIES], cv; } g_imp[IMP_MAX];   // v[0]: at 30 fps, v[1]: at 60; cop 0 = no compare
static int g_nimp;
static int imp_type(const char *s) { for (int t = 0; t < T_N; t++) if (!strcmp(s, k_tname[t])) return t; return -1; }
static int imp_chain_parse(char *s, imp_chain *c) {   // "0x7a642d8" or "0x1ba1a90,0x80,-0x4"
    c->n = 0;
    for (char *tok; c->n < IMP_CHAIN && (tok = strsep(&s, ",")); ) { char *end; c->off[c->n++] = strtoll(tok, &end, 0); if (end == tok || *end) return 0; }
    return c->n > 0 && !s;
}
static int imp_value(const char *s, int t, uint64_t *bits) {   // one number of type t as its bits, zero-extended
    char *end; *bits = 0;
    if (t == T_F32) { float f = strtof(s, &end); memcpy(bits, &f, 4); }
    else if (t == T_F64) { double d = strtod(s, &end); memcpy(bits, &d, 8); }
    else { uint64_t v = t >= T_U8 ? strtoull(s, &end, 0) : (uint64_t)strtoll(s, &end, 0); *bits = k_tsize[t] == 8 ? v : v & ((1ull << 8 * k_tsize[t]) - 1); }
    return end != s && !*end;
}
static int imp_values(char *s, int t, uint64_t *out) {   // "1,1": the copies; their count, 0 when malformed
    int n = 0;
    for (char *tok; (tok = strsep(&s, ",")); ) { if (n == IMP_COPIES || !imp_value(tok, t, &out[n])) return 0; n++; }
    return n;
}
static void imp_load(const char *build) {
    const char *path = getenv("DM_FPS_LOCKS"); if (!path || !*path) return;
    FILE *f = fopen(path, "r"); if (!f) { LOG("[RED] DM_FPS_LOCKS=%s: cannot open it; the game keeps its frame limit", path); return; }
    char line[1024]; int ln = 0, ok = 0;
    while (fgets(line, sizeof line, f)) {
        ln++; line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        if (!strncmp(line, "build\t", 6)) { if (!(ok = !strcmp(line + 6, build))) { LOG("[RED] %s: rows for main build %s, the game's is %s; not used", path, line + 6, build); break; } continue; }
        if (!ok) { LOG("[RED] %s:%d: a row before the build line; not used", path, ln); break; }
        if (g_nimp == IMP_MAX) { LOG("[RED] %s: more than %d rows; the rest left out", path, IMP_MAX); break; }
        char *s = line, *fld[6]; int nf = 0;
        while (nf < 6 && (fld[nf] = strsep(&s, "\t"))) nf++;
        typeof(g_imp[0]) *r = &g_imp[g_nimp]; memset(r, 0, sizeof *r);
        int t = nf == 6 && !s ? imp_type(fld[1]) : -1, n30 = 0, n60 = 0;
        if (t >= 0) { r->type = (uint8_t)t; n30 = imp_values(fld[3], t, r->v[0]); n60 = imp_values(fld[4], t, r->v[1]); }
        int good = t >= 0 && imp_chain_parse(fld[2], &r->a) && n30 && n30 == n60;
        if (good && strcmp(fld[5], "-")) {   // "<type> <offsets> <operator> <value>"
            static const char *const ops[] = {"<", "<=", ">", ">=", "==", "!="}; char *c = fld[5], *ct = strsep(&c, " "), *co = strsep(&c, " "), *op = strsep(&c, " "), *cv = c;
            int ctp = ct ? imp_type(ct) : -1; good = ctp >= 0 && co && op && cv && imp_chain_parse(co, &r->c) && imp_value(cv, ctp, &r->cv);
            if (good) { r->ctype = (uint8_t)ctp; for (int k = 0; k < 6; k++) if (!strcmp(op, ops[k])) r->cop = (uint8_t)(k + 1); good = r->cop != 0; }
        }
        if (!good) { LOG("[RED] %s:%d: not a row of type, offsets, values at 30 and 60, compare; left out", path, ln); continue; }
        r->ncopy = (uint8_t)n30; snprintf(r->what, sizeof r->what, "%s", fld[0]); g_nimp++;
    }
    fclose(f);
    if (g_nimp) LOG("main: %d frame limit write(s) from %s (FPSLocker-Warehouse config)", g_nimp, path);
}
static uint64_t imp_addr(uint8_t *base, const imp_chain *c) {   // main + off[0], then through each pointer; 0 = a pointer not there yet
    uint64_t a = (uint64_t)(uintptr_t)base + (uint64_t)c->off[0];
    for (int k = 1; k < c->n; k++) { uint64_t p; if (!rd(a, &p, sizeof p) || !p) return 0; a = p + (uint64_t)c->off[k]; }
    return a;
}
static int imp_holds(uint8_t *base, int i) {
    typeof(g_imp[0]) *r = &g_imp[i]; if (!r->cop) return 1;
    uint64_t a = imp_addr(base, &r->c), bits = 0; int t = r->ctype;
    if (!a || !rd(a, &bits, k_tsize[t])) return 0;
    int cmp;   // the field against the value, by the type's order
    if (t == T_F32 || t == T_F64) { double x, y; if (t == T_F32) { float fx, fy; memcpy(&fx, &bits, 4); memcpy(&fy, &r->cv, 4); x = fx; y = fy; } else { memcpy(&x, &bits, 8); memcpy(&y, &r->cv, 8); } cmp = x < y ? -1 : x > y; }
    else if (t >= T_U8) cmp = bits < r->cv ? -1 : bits > r->cv;
    else { int sh = 64 - 8 * k_tsize[t]; int64_t x = (int64_t)(bits << sh) >> sh, y = (int64_t)(r->cv << sh) >> sh; cmp = x < y ? -1 : x > y; }
    switch (r->cop) { case 1: return cmp < 0; case 2: return cmp <= 0; case 3: return cmp > 0; case 4: return cmp >= 0; case 5: return cmp == 0; default: return cmp != 0; }
}
static void imp_tick(uint8_t *base, int target) {
    static int nlog[IMP_MAX];
    for (int i = 0; i < g_nimp; i++) {
        typeof(g_imp[0]) *r = &g_imp[i]; int sz = k_tsize[r->type];
        if (!imp_holds(base, i)) continue;
        uint64_t a = imp_addr(base, &r->a); if (!a) continue;
        for (int c = 0; c < r->ncopy; c++) {
            uint64_t cur = 0, want = r->v[target == 60][c]; uint8_t *p = (uint8_t *)(uintptr_t)(a + (uint64_t)c * (uint64_t)sz);
            if (!rd((uint64_t)(uintptr_t)p, &cur, (size_t)sz)) break;
            if (cur == want) continue;
            dm_guest_write_prepare(p, (size_t)sz); memcpy(p, &want, (size_t)sz);
            if (nlog[i] < 8) { nlog[i]++; LOG("main: %s (%s at %#llx%s) %#llx → %#llx (%d fps)", r->what, k_tname[r->type], (unsigned long long)(a - (uint64_t)(uintptr_t)base),
                                              r->a.n > 1 ? " through pointers" : "", (unsigned long long)cur, (unsigned long long)want, target); }
        }
    }
}

// DM_UE_CVARS (start-up; a profile env line): Unreal console variables held at a value, "<main build id>:<main offset>=f<float>|i<int>,..." with the offset of the
// TAutoConsoleVariable object (its value pointer at +0x10: the game thread's copy, then the render thread's), written whenever the game sets them otherwise
// (a level load reads the config again). For a setting the shipping build takes from no command line. FINAL FANTASY RESONANCE: the dynamic resolution's floor
static void cvars_tick(void) {
    static int n = -1; static struct { uint64_t off; int is_f; uint32_t bits; int logged; } c[16]; static uint8_t *base;
    if (n < 0) { n = 0; const char *e = getenv("DM_UE_CVARS"); if (!e || !*e) return;
        const char *colon = strchr(e, ':'); const char *bid = "";
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) { base = g_mod[i].base; bid = g_mod[i].build_id; }
        if (!colon || strncmp(e, bid, (size_t)(colon - e)) || (size_t)(colon - e) != strlen(bid)) { LOG("DM_UE_CVARS: for another main build (%.*s, this is %s): not used", colon ? (int)(colon - e) : 0, e, bid); base = NULL; return; }
        char *s = strdup(colon + 1);
        for (char *t = strtok(s, ","); t && n < 16; t = strtok(NULL, ",")) { char *eq = strchr(t, '='); if (!eq || (eq[1] != 'f' && eq[1] != 'i')) { LOG("[RED] DM_UE_CVARS: \"%s\" is not <offset>=f<value> or i<value>", t); continue; }
            c[n].off = strtoull(t, NULL, 0); c[n].is_f = eq[1] == 'f';
            if (c[n].is_f) { float f = strtof(eq + 2, NULL); memcpy(&c[n].bits, &f, 4); } else { int32_t v = (int32_t)strtol(eq + 2, NULL, 0); memcpy(&c[n].bits, &v, 4); }
            n++; }
        free(s); }
    for (int k = 0; k < n && base; k++) {
        uint64_t obj; if (!rd((uint64_t)(uintptr_t)(base + c[k].off + 0x10), &obj, sizeof obj) || !obj) continue;   // not registered yet
        for (int cp = 0; cp < 2; cp++) { uint32_t *p = (uint32_t *)(uintptr_t)(obj + 4 * (uint64_t)cp), cur;
            if (!rd((uint64_t)(uintptr_t)p, &cur, 4) || cur == c[k].bits) continue;
            dm_guest_write_prepare(p, 4); __atomic_store_n(p, c[k].bits, __ATOMIC_RELAXED);
            if (c[k].logged < 4) { c[k].logged++; float f; int32_t v; memcpy(&f, &c[k].bits, 4); memcpy(&v, &c[k].bits, 4);
                if (c[k].is_f) LOG("main: console variable at main+%#llx (%s) set to %g (DM_UE_CVARS)", (unsigned long long)c[k].off, cp ? "render thread" : "game thread", (double)f);
                else LOG("main: console variable at main+%#llx (%s) set to %d (DM_UE_CVARS)", (unsigned long long)c[k].off, cp ? "render thread" : "game thread", v); } }
    }
}
void dm_fps_lock_tick(void) {
    cvars_tick();
    static int state = -2, rows[NLOCKS], nrow, nlog[NLOCKS], target; static uint8_t *base; static const char *src;
    if (state == -1) return;
    if (state == -2) {
        state = -1;
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
            for (int k = 0; k < NLOCKS; k++) if (!strcmp(g_mod[i].build_id, k_locks[k].build)) { rows[nrow++] = k; base = g_mod[i].base; }
            if (nrow && getenv("DM_FPS_LOCKS")) LOG("main: the host's own frame limit rows for this build; %s not used", getenv("DM_FPS_LOCKS"));
            else if (!nrow) { imp_load(g_mod[i].build_id); if (g_nimp) base = g_mod[i].base; }
        }
        if (!nrow && !g_nimp) return;
        const char *e = getenv("DM_FPS_TARGET"); int t = e ? atoi(e) : DM_GAME_FPS_TARGET; src = e ? "DM_FPS_TARGET" : "game profile fps_target";
        if (t != 0 && t != 30 && t != 60) { LOG("[RED] DM_FPS_TARGET=%s: must be 0, 30 or 60; leaving the game's frame limit", e); t = 0; }
        if (!t) { for (int r = 0; r < nrow; r++) LOG("main: %s left as the game sets it (%s)", k_locks[rows[r]].what, src);
                  if (g_nimp) LOG("main: %d FPSLocker write(s) left out (%s 0)", g_nimp, src); return; }
        target = t; state = 0;
    }
    if (g_nimp) { imp_tick(base, target); return; }
    for (int r = 0; r < nrow; r++) {
        int k = rows[r]; uint64_t obj = (uint64_t)(uintptr_t)base;
        if (k_locks[k].ptr_off && (!rd((uint64_t)(uintptr_t)(base + k_locks[k].ptr_off), &obj, sizeof obj) || !obj)) continue;   // not created yet
        if (k_locks[k].ptr2 && (!rd(obj + k_locks[k].ptr2, &obj, sizeof obj) || !obj)) continue;   // the class's static fields not allocated yet
        for (int c = 0; c < k_locks[k].copies; c++) {
            uint64_t a = obj + k_locks[k].field + 4 * (uint64_t)c; uint32_t cur, want; char was[16];
            if (!rd(a, &cur, sizeof cur)) break;
            if (k_locks[k].kind != VSYNCS_I32) {
                int ms = k_locks[k].kind == MS_F32 ? 1 : k_locks[k].kind == SEC_F32 ? 1000 : 0; float f, w = ms ? ms_want(target) / (float)ms : (float)target; memcpy(&f, &cur, sizeof f); memcpy(&want, &w, sizeof want);
                if (cur == want || !(ms ? f * (float)ms >= 8 && f * (float)ms <= 100 : f >= 10 && f <= 120)) continue;   // already set, or not set yet (RE Engine: 6000 at boot; Unreal: 0 = no limit until the config sets it)
                snprintf(was, sizeof was, ms ? "%.3f ms" : "%g", ms ? f * (float)ms : f);
            } else {
                want = 60 / (uint32_t)target;
                if (cur == want || cur < 1 || cur > 4) continue;
                snprintf(was, sizeof was, "%u", cur);
            }
            uint32_t *p = (uint32_t *)(uintptr_t)a;
            dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, want, __ATOMIC_RELAXED);
            if (nlog[k] < 8) { nlog[k]++; char now[24];
                if (k_locks[k].kind == MS_F32 || k_locks[k].kind == SEC_F32) snprintf(now, sizeof now, "%.3f ms (%d fps)", ms_want(target), target);
                else snprintf(now, sizeof now, "%u%s", k_locks[k].kind == FPS_F32 ? (unsigned)target : want, k_locks[k].kind == FPS_F32 ? " fps" : " vsyncs");
                LOG("main: %s%s %s → %s (%s)", k_locks[k].what, k_locks[k].copies > 1 ? (c ? " (render thread)" : " (game thread)") : "", was, now, src); }
        }
    }
}

// The Mac's DM_CTL "cvarf <main offset> <value>" (hle_hid.c), for trying an Unreal console variable while the game runs: the TAutoConsoleVariable at main+offset holds at +0x10
// the pointer to its value, the game thread's copy then the render thread's; both are set to the float
void dm_ctl_cvar_f32(uint64_t off, float v) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        uint64_t obj; if (!rd((uint64_t)(uintptr_t)(g_mod[i].base + off + 0x10), &obj, sizeof obj) || !obj) { LOG("DM_CTL: cvarf main+%#llx: no value pointer yet", (unsigned long long)off); return; }
        for (int c = 0; c < 2; c++) { float *p = (float *)(uintptr_t)(obj + 4 * (uint64_t)c); dm_guest_write_prepare(p, sizeof *p); *p = v; }
        LOG("DM_CTL: cvarf main+%#llx = %g (both copies at %#llx)", (unsigned long long)off, v, (unsigned long long)obj);
    }
}
// "peek <guest address> <count>": count floats (and their bits) from guest memory, read safely, to the log
void dm_ctl_peek(uint64_t a, int n) {
    for (int i = 0; i < n && i < 64; i += 4) {
        uint32_t w[4] = {0}; int k = n - i < 4 ? n - i : 4;
        if (!rd(a + 4 * (uint64_t)i, w, 4 * (size_t)k)) { LOG("DM_CTL: peek %#llx: not readable", (unsigned long long)(a + 4 * (uint64_t)i)); return; }
        float f[4]; memcpy(f, w, sizeof f);
        LOG("DM_CTL: peek %#llx: %g %g %g %g (%08x %08x %08x %08x)", (unsigned long long)(a + 4 * (uint64_t)i), f[0], f[1], f[2], f[3], w[0], w[1], w[2], w[3]);
    }
}
// "memdump <guest address> <bytes> <path>": guest memory to a file, read safely (stops at the first unreadable page)
void dm_ctl_memdump(uint64_t a, uint64_t n, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) { LOG("[RED] DM_CTL: memdump: cannot open %s", path); return; }
    static uint8_t buf[1 << 16]; uint64_t done = 0;
    while (done < n) { size_t k = n - done < sizeof buf ? (size_t)(n - done) : sizeof buf; if (!rd(a + done, buf, k)) break; fwrite(buf, 1, k, f); done += k; }
    fclose(f); LOG("DM_CTL: memdump %#llx: %llu of %llu bytes → %s", (unsigned long long)a, (unsigned long long)done, (unsigned long long)n, path);
}
// "cvari <main offset> <value>": the same for an int console variable
void dm_ctl_cvar_i32(uint64_t off, int32_t v) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        uint64_t obj; if (!rd((uint64_t)(uintptr_t)(g_mod[i].base + off + 0x10), &obj, sizeof obj) || !obj) { LOG("DM_CTL: cvari main+%#llx: no value pointer yet", (unsigned long long)off); return; }
        for (int c = 0; c < 2; c++) { int32_t *p = (int32_t *)(uintptr_t)(obj + 4 * (uint64_t)c); dm_guest_write_prepare(p, sizeof *p); *p = v; }
        LOG("DM_CTL: cvari main+%#llx = %d (both copies at %#llx)", (unsigned long long)off, v, (unsigned long long)obj);
    }
}
