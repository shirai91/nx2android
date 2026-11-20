// Common header of the native Switch host (multi-module).
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_profile.h"   // constants of the current game profile (generated from dm/games/<id>/game.tsv, chosen by the Makefile's GAME)

#define MAX_MOD 128   // the profile's modules (up to 8) and the NROs a game loads at run time (Super Smash Bros. Ultimate: 98)
// Seconds since process start, the same origin the fs read log uses (hle_fs.c's g_t0_ns), so log.txt and fslog.tsv line up. g_t0_ns is set once by
// host_preload; now_ns() is a clock read, where proc_start_mono_ns() on Android walks /proc through the sysctl shim for every line.
double dm_log_elapsed(void);   // defined in dm/host/main.c
// One line at a time: LOG is called from guest threads and from the replay/present threads at once, and two stdio calls per line let another
// thread's text land in the middle of this one. flockfile keeps it whole without formatting into a buffer (a guest thread's stack can be 8 KB)
#define LOG(...) do { flockfile(stderr); fprintf(stderr, "[dm] %9.3fs ", dm_log_elapsed()); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); funlockfile(stderr); } while (0)
#define die(...) do { LOG("FATAL " __VA_ARGS__); abort(); } while (0)

// Object-format differences for inline assembly and registration tables (Android port: ELF instead of Mach-O).
// DM_ASM_SYM(name): a C symbol as assembly names it; DM_ASM_PAGE / DM_ASM_PAGEOFF: the adrp / add operands for its address; DM_ASM_PRIVATE: a global symbol
// kept inside the image. DM_SECTION(name): a registration table section, found again by getsectiondata (on ELF the linker bounds it with __start_ / __stop_ symbols)
#if defined(__APPLE__)
#define DM_ASM_SYM(n) "_" n
#define DM_ASM_PAGE(n) "_" n "@PAGE"
#define DM_ASM_PAGEOFF(n) "_" n "@PAGEOFF"
#define DM_ASM_PRIVATE ".private_extern"
#define DM_SECTION(s) section("__DATA," s)
#else
#define DM_ASM_SYM(n) n
#define DM_ASM_PAGE(n) n
#define DM_ASM_PAGEOFF(n) ":lo12:" n
#define DM_ASM_PRIVATE ".hidden"
#define DM_SECTION(s) section(s), retain
#endif

typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { int64_t d_tag; union { uint64_t d_val, d_ptr; } d_un; } Elf64_Dyn;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
enum { DT_INIT = 12, DT_FINI = 13, DT_RELA = 7, DT_RELASZ = 8, DT_JMPREL = 23, DT_PLTRELSZ = 2, DT_SYMTAB = 6, DT_STRTAB = 5,
       DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27, DT_RELRSZ = 35, DT_RELR = 36 };
enum { R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027 };
#define ELF64_ST_BIND(i) ((i) >> 4)            // symbol binding: 0 = STB_LOCAL, 2 = STB_WEAK
#define ELF64_ST_VISIBILITY(o) ((o) & 3)       // visibility: 0 = STV_DEFAULT
enum { STB_LOCAL = 0, STB_WEAK = 2, STV_DEFAULT = 0 };

// Run-time state of one NSO module
typedef struct {
    char name[16];
    uint8_t *base;             // Start address of the image (including the patch area) in the host address space
    size_t size;
    uint64_t text_end, ro_off, data_off, key_page_off, key_page_size;   // From the rewriter fixups header
    uint64_t fps_state_off;    // frame step state page (patch_nso.py FRAME_STEP: Tears of the Kingdom 1.4.3 and Breath of the Wild 1.9.0 main); 0 = none
    int64_t patch_off;         // patch area relative to the base; negative = before the base (image too large, see patch_nso.py FAR), and size does not include it
    uint64_t image_size, file_image_len, patch_len;   // only with the patch area before the base: whole image (with bss and key page), length of the image part of the file, patch area length
    char build_id[33];         // First 16 bytes of the NSO build ID (hex); empty for older build outputs
    unsigned key_idx[4];       // key page index of each thread slot (K18, KTLS, KTLR, KGATE)
    const Elf64_Dyn *dyn;
    const Elf64_Sym *symtab;
    const char *strtab;
    unsigned nsym;
    uint32_t *symh; unsigned symh_mask;   // Name hash table of defined symbols (open addressing, stores symbol indices; for a duplicated name only the smallest index is kept = the first hit of a linear scan)
    uint64_t init, init_array, init_arraysz;   // init = DT_INIT (the module's _init: registers the DSO and runs this module's init_array)
    uint64_t fini;             // DT_FINI (nn::ro::UnloadModule calls it)
    // NRO modules (nn::ro, loader.c "NRO modules"): mapped at start like the others, but they take part in symbol lookups only between LoadModule and UnloadModule
    int nro, loaded;           // loaded: the profile's modules always; an NRO while the game has it loaded
    uint8_t sha256[32];        // an NRO's SHA-256 (what the game's NRR registers; LoadModule finds the module by the image it passes)
    uint32_t nro_size;         // an NRO's file size (header +0x18), the bytes the hash covers
    uint8_t *pristine; size_t pristine_len;   // an NRO's data and bss as rewritten, restored before each load after the first
    unsigned loads;            // how many times the game loaded it
} module_t;

extern module_t g_mod[MAX_MOD];
extern int g_nmod;
// main's build ID (empty if none): code with fixed addresses in a game's module runs only on the build they were found in
static inline const char *dm_main_build(void) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return g_mod[i].build_id; return ""; }
// Four thread-local slots: K18 = the Switch's x18, KTLS = tpidr_el0, KTLR = tpidrro_el0 (ThreadLocalRegion), KGATE = host gate entry; the actual key numbers are written into the key page by the loader
extern pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
// The guest thread slots the rewritten code reads (patch_nso.py emit_tp): Darwin TSD keys on Apple; on Android the keys are indices into a per-thread
// block whose address bionic's TLS_SLOT_APP holds (tpidr_el0[2]), see loader.c
#if defined(__APPLE__)
#define dm_tsd_set(k, v) pthread_setspecific((k), (v))
#define dm_tsd_get(k) pthread_getspecific(k)
#else
void dm_tsd_set(pthread_key_t k, const void *v);
void *dm_tsd_get(pthread_key_t k);
#endif

void load_modules(const char *dir, const char **names, int n);
const char *dm_module_dir(void);   // the folder load_modules read the modules and the rewriter's tables from (the cheat engine reads main.changed.tsv there)
void run_module_inits(void);
module_t *nro_find(const uint8_t sha256[32]);   // the mapped NRO with this hash (loader.c)
int nro_refused(const uint8_t sha256[32]);      // an NRO the pack refused (an online SDK, nro.tsv "refused")
void nro_load(module_t *m);     // relocate an NRO against the loaded modules and bind the earlier modules' imports it defines (the caller runs its init)
void nro_unload(module_t *m);   // take an NRO out of the lookups and bind the imports that pointed into it again
void *module_sym(module_t *m, const char *name);
int dm_sdk_major(void);   // SDK major version from the sdk module's version string (loader.c); 0 = unknown
#define DM_SDK_OLD(major) ((major) > 0 && (major) <= 10)   // SDK 10 and older: NVN API 53, the shared-font address points at the TTF itself
int dm_main_imports(const char *prefix);   // main imports a symbol whose name starts with prefix (loader.c)
void *guest_sym(const char *name);
module_t *module_of(const void *p);
// Gate dispatch: rewritten svc / timer reads come in through the common entry, ret_addr = the return point of that gate block, regs points to the saved x0..x30
uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs);
// Observation gates (rewriter OBS tables in the profiles' patches.py, applied by patch_nso.py; numbering matches): read-only callbacks, called on a gate hit once attached
enum { OBS_MOVIE_RING_LINK = 19, OBS_MOVIE_RING_UNLINK = 20,   // Metroid Dread 2.1.0's movie request ring (hle_movie.c)
       OBS_UE_MALLOC = 21,                                       // DRAGON QUEST VII Reimagined 1.2.0: Unreal's allocator object just made (hle_uemalloc.c)
       OBS_MAX = 22 };
void gate_obs_set(int id, void (*fn)(const uint64_t *regs));
int gate_obs_present(int id);
uint64_t now_ns(void);
uint64_t guest_ns(void);              // Guest clock (hle_os.c; does not advance while frozen, guest_clock_freeze)
int dm_detclock_enabled(void);
void dm_guest_clock_advanced(void);
void guest_clock_freeze(int on);
void guest_clock_freeze_for(int reason, int on);   // Freeze by reason (1 background, 2 system keyboard); see hle_os.c
void guest_clock_read_wait(void);     // A guest read of the system counter: waits while the app is in the background (hle_os.c)
void dm_guest_frame_tick(void);        // Called on every present (hle_nvn.c): when DM_DETCLOCK is on, advances the guest clock by one fixed step
uint64_t dm_nvn_frame_count(void);
uint64_t dm_nvn_sub_cpu_ns(void);     // Cumulative CPU nanoseconds of submit (excluding present)
void dm_backtrace(uint64_t fp);
int crash_addr_str(char *b, size_t n, uint64_t a);   // "module+off", "symbol+off" or "library+off"
uint64_t crash_code_addr(uint64_t a);   // a return address without its pointer authentication code
void crash_thread_name(unsigned t, char *b, size_t n);   // by thread port (the tid on Android)
void crash_add_note_hook(void (*fn)(void));   // printed after the crashing thread's registers
void crash_watch_progress(uint32_t tick, const char *stage);
void crash_watch_frame(void);
void crash_watch_pregame_done(void);
uint64_t crash_watch_progress_count(void);
void crash_dump_now(const char *why);   // Dump state on demand (threads + graphics wait points), without exiting
// Diagnostic switches: read once from environment variables when main starts (getenv takes the libc global environment lock, and on a hot path it would make all guest threads queue up)
extern uint64_t g_t0_ns;   // Host start time (now_ns), used for relative times in the log
extern struct dm_flags { int thr, svc, fs, movie, dbg, imports, bootprof; } g_flag;
void *objmap_get(const void *guest); void objmap_put(const void *guest, void *host); void *objmap_del(const void *guest);
void hle_signal_system_event(void *g);   // hle_thread.c: signal a service-layer SystemEvent (any thread)
void hle_bind_system_event(void *g, int autoclear);   // On a guest thread, bind a guest SystemEvent to a kernel event
void hle_rebind_system_event(void *g, int autoclear);   // the same, dropping a binding this address had (a struct reused without being destroyed)
void hle_clear_system_event(void *g);    // hle_thread.c: clear a bound service-layer SystemEvent (any thread)
int hle_signal_bound_system_event(void *g);   // hle_thread.c: signal a service-layer SystemEvent only if it is bound (any thread); 0 when it is not
// Page fault hook / hook before the host writes guest memory (crash.c; used for write-protect dirty page tracking)
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc));
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len));
void dm_res_report_register(void (*fn)(uint32_t tick, const char *why));   // Resource count callback (registered by the graphics layer): called every 250 ticks, when the loading screen has been dismissed, and on exit when the record is full
void (*dm_res_report_cb(void))(uint32_t tick, const char *why);   // Get the registered resource count callback (null if not registered)
void dm_guest_write_prepare(void *addr, size_t len);
void dm_guest_heap_range(uint64_t *base, size_t *open_len);   // hle_svc.c: the guest heap's base and opened length (guest addresses), 0 / 0 when none
int dm_guest_heap_read(uint64_t addr, void *out, size_t len);    // hle_svc.c: hole-free guest heap read for the cheat VM (under the memory lock); 0 = ok, 1 = a byte outside the opened span
int dm_guest_heap_write(uint64_t addr, const void *src, size_t len);   // hle_svc.c: hole-free guest heap write (the dirty tracking sees it first)
// Cheats (hle_cheats.c): Atmosphère-format cheat code files applied to the running game (docs/reference/cheats.md)
void dm_cheats_init(void);            // Called once in host_preload after loading the modules (main.c): reads DM_CHEATS and friends, loads the build's file
void dm_cheats_present(void);         // Per presented frame (hle_nvn.c presents): applies the enabled cheats once
// The rows a front end shows. Readers of the list of cheats; the front end copies the strings out immediately
int dm_cheats_found(void);            // A cheat file is loaded (this build's, or another build's on DM_CHEATS_ANY_BUILD)
int dm_cheats_ncheats(void);          // The loaded file's cheats, in file order
const char *dm_cheats_build(void);    // This game's build ID, first 16 hex digits ("" when the main NSO has none)
const char *dm_cheats_dir_path(void); // The cheats folder ("", when neither DM_CHEATS nor DM_ROOT is set: the menu's folder action is off)
int dm_cheats_nothers(void);          // The .txt files of other builds in the folder (the menu's disabled "another version" rows)
const char *dm_cheats_other(int i);
const char *dm_cheat_name_at(int i);
const char *dm_cheat_why_at(int i);   // The disabled state's reason (" (unsupported opcode N)", " (off: ...)"); "" when plain
int dm_cheat_kind_at(int i);          // 0 a plain row (click toggles), 1 master (always on, no toggle), 2 disabled (broken or killed at run time), 3 a label (credits, a section marker)
int dm_cheat_on_at(int i);
void dm_cheat_toggle(int i);          // The click on a plain row: flips the cheat (an on / off pair as one switch) and rewrites <build ID>.enabled
void dm_cheats_reload(void);          // "Reload cheats": re-reads the file (the same path), so edits during play need no re-launch
// What the panel shows beyond the menu (hle_cheats.c header: labels and sections, the meta file of scripts/cheat_anchor.py)
enum { DM_CHEAT_LABEL = 1, DM_CHEAT_SECTION_START = 2, DM_CHEAT_PAIR_ON = 4, DM_CHEAT_PAIR_OFF = 8,
       DM_CHEAT_ONCE = 16, DM_CHEAT_ASK = 32, DM_CHEAT_HOST = 64, DM_CHEAT_WEAK = 128 };   // ONCE: a "set" cheat; HOST: a never-decrease run by this host only
int dm_cheat_flags_at(int i);         // DM_CHEAT_* of entry i
const char *dm_cheat_section_at(int i);   // the section entry i lies in ("" none)
const char *dm_cheat_note_at(int i);  // the meta file's note: how the cheat was compiled, and its own note
const char *dm_cheat_type_at(int i);  // the meta type (u16, s32, f32, …), "" unknown
int dm_cheat_pair_at(int i);          // the other half of an on / off pair, -1 none
int dm_cheat_preview(int i, char *out, size_t cap);   // a dry run: one line per write it would make now ("main+0x… (2 bytes): 37 -> 999"), nothing written; returns the writes
int dm_cheat_value_get(int i, char *out, size_t cap); // an "ask" cheat's value as text: 1, or 0 when it takes none
int dm_cheat_value_set(int i, const char *text);      // set it (its type, its bounds) and keep it in <build ID>.values: 0 done, -1 refused
const char *dm_cheat_value_bounds(int i);             // "min..max" (a side empty when open), "" when it takes none
int dm_cheats_master(void);           // Enable cheats (the Cheats tab's switch above the list): 0 = no cheat runs, the masters too; every launch starts there
void dm_cheats_master_set(int on);    // turning it on applies the saved toggles for this run; off first puts back the main-text words the passes changed
const char *dm_cheats_report(void);   // the load report: counts, duplicate names, ignored lines, carried-over toggles, Atmosphère's limits
const char *dm_cheats_version(void);  // this build's display version ("" unknown)
const char *dm_cheats_other_version(int i);   // the display version of another build's file (its meta file), "" unknown
int dm_cheats_nskipped(void);         // cheats the compiler left out of this build's file (its meta file), with their reasons
const char *dm_cheats_skipped_name(int i);
const char *dm_cheats_skipped_why(int i);
int dm_cheats_nsources(void);         // anchored sources (*.cheats.toml) in the cheats folder
const char *dm_cheats_source(int i);
void dm_cheats_exit(void);            // a clean exit (hle_lifecycle.c, atexit): the queued state files land before the process ends
// Graphics lifecycle (hle_nvn.c): on=1 suspends (afterwards submit / present block, and in-flight GPU work is drained before returning), on=0 resumes; called by the iOS front end when entering and leaving the background
void dm_gfx_suspend(int on);
void dm_movie_hold(int on);   // hle_movie.c: the game is held (background, pause menu); the time held is no movie decoder's idle time (md_tail)
void dm_gfx_enter(void); void dm_gfx_leave(void);   // hle_nvn.c: the gate dm_gfx_suspend closes, for graphics layers other than NVN (dm/gfx/vk)
void dm_gfx_shot_at(long k, const char *path);
void dm_shot_written(const char *path, int ok);   // The graphics side finished writing a screenshot to disk (diag.c: DM_TICK_HOLD=E:SHOT unfreezes on this)
void dm_host_shutdown(const char *why);   // Graceful exit (crash.c): stop background threads and audio, drain graphics, then _exit(0); used by SIGTERM and the iOS App termination callback   // graphics side: screenshot of the k-th frame after the current presented frame (nvn_mtl.m)
