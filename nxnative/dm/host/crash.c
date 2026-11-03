#include <TargetConditionals.h>
// Crash state: prints the faulting address, pc, lr (converted to module-relative offsets) and the registers; dispatches page fault hooks.
// Only signal handling itself lives here: diagnostic features (probes / UI messages / sampling) are in diag.c, and this file does not depend on them (the graphics unit tests link only this file).
#include "dm.h"
#include "dm_uc.h"
#include <signal.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>

static _Atomic uint64_t g_watch_last_ns;
static _Atomic uint64_t g_watch_count;
static _Atomic uint32_t g_watch_tick;
static _Atomic int g_watch_pregame = 1;
static _Atomic int g_watch_stage_idx;
static char g_watch_stage[2][256] = {"startup", "startup"};
static unsigned g_watch_stall;
static void (*g_dump_hook)(void);
static void (*g_wait_dump)(void);

static uint64_t watch_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t crash_watch_progress_count(void) { return atomic_load(&g_watch_count); }
void crash_watch_progress(uint32_t tick, const char *stage) {
    int next = 1 - atomic_load(&g_watch_stage_idx);
    if (stage && *stage) snprintf(g_watch_stage[next], sizeof g_watch_stage[next], "%s", stage);
    atomic_store(&g_watch_tick, tick);
    atomic_store(&g_watch_stage_idx, next);
    atomic_fetch_add(&g_watch_count, 1);
    atomic_store(&g_watch_last_ns, watch_now_ns());
}
void crash_watch_frame(void) { if (atomic_load(&g_watch_pregame)) crash_watch_progress(0, "present frame"); }
void crash_watch_pregame_done(void) { atomic_store(&g_watch_pregame, 0); }
void crash_watch_pregame_again(void) { atomic_store(&g_watch_pregame, 1); }   // Back from a run to the front end: count progress by presented frames again (diag.c)
void crash_dump_now(const char *why);
static void *watchdog_main(void *arg) {
    (void)arg;
    for (;;) {
        usleep(250000);
        // Read the last progress time first, then read now: reading in the opposite order, if the game thread happens to record progress between the two reads, last is newer than now and now − last underflows as unsigned into "no progress since second 18446744073",
        // which is misjudged on the spot as a hang and exits (accept inv_drag_out, local load 11, the watchdog thread was switched out between the two reads). Reading this way gives last ≤ now; and if the clock ever wraps around it is not judged either
        uint64_t last = atomic_load(&g_watch_last_ns), now = watch_now_ns();
        if (last > now) continue;
        // Second-scale stall (progress has stopped for more than 1 second but has not yet reached the kill threshold): do not exit, dump what each thread is waiting on. When hitting a case of "only one frame every 2 seconds", the 15-second threshold cannot catch it and the log has nothing on what it is waiting for.
        // Dump only after entering a run; at most 3 dumps per process, at least 10 seconds between two dumps (the loading screen and the pause menu also stop ticks, do not let them flood the log)
        { static int dumps; static uint64_t last_dump, dumped_for;
          if (g_watch_stall && !atomic_load(&g_watch_pregame) && atomic_load(&g_watch_tick) > 0 && now - last >= 1000000000ull && now - last < (uint64_t)g_watch_stall * 1000000000ull
              && dumps < 3 && dumped_for != last && (!last_dump || now - last_dump >= 10000000000ull)) { dumps++; last_dump = now; dumped_for = last; crash_dump_now("progress stalled for over 1 second"); } }
        if (g_watch_stall && now - last >= (uint64_t)g_watch_stall * 1000000000ull) {
            uint64_t sec = (now - last) / 1000000000ull;
            int idx = atomic_load(&g_watch_stage_idx);
            LOG("[RED] watchdog: no progress since second %llu, stuck at tick %u / stage %s", (unsigned long long)sec,
                atomic_load(&g_watch_tick), g_watch_stage[idx]);
            if (g_dump_hook) g_dump_hook();
            if (g_wait_dump) g_wait_dump();   // State of all blocking wait points on the graphics side (registered when nvn_mtl.m initializes)
#if TARGET_OS_OSX
            { const char *d = getenv("DM_WATCH_SAMPLE");   // Mac diagnostics: when stuck, sample itself for 1 second and write the thread stacks to this path
              if (d) { char c[1200]; snprintf(c, sizeof c, "/usr/bin/sample %d 1 -f '%s' >/dev/null 2>&1", (int)getpid(), d); if (system(c)) LOG("watchdog: sampling failed"); else LOG("watchdog: thread stacks written to %s", d); } }
#endif
            _exit(128 + SIGALRM);
        }
    }
    return NULL;
}
// An address as text: a guest module offset, a symbol of the host's own library, or library+offset for any other library (with its nearest exported symbol when it has one).
// The offset is the library's own address, as a disassembler of that file shows it: issue #64's crash in the Adreno driver's shader compiler (libllvm-qgl.so, stripped) printed
// a bare pc, and the reporter had to find the library and the offset from /proc/<pid>/maps by hand. Also used by the thread dump (hle_thread.c)
int crash_addr_str(char *b, size_t n, uint64_t a) {
    module_t *m = module_of((void *)a); Dl_info di, me;
    if (m) return snprintf(b, n, "%s+%#llx", m->name, a - (uint64_t)m->base);
    if (!dladdr((void *)a, &di) || !di.dli_fname) return snprintf(b, n, "%#llx", a);
    if (di.dli_sname && dladdr((void *)crash_addr_str, &me) && me.dli_fbase == di.dli_fbase) return snprintf(b, n, "%s+%#lx", di.dli_sname, (unsigned long)(a - (uint64_t)di.dli_saddr));
    const char *f = strrchr(di.dli_fname, '/'); f = f ? f + 1 : di.dli_fname;
    if (di.dli_sname) return snprintf(b, n, "%s+%#lx(%s+%#lx)", f, (unsigned long)(a - (uint64_t)di.dli_fbase), di.dli_sname, (unsigned long)(a - (uint64_t)di.dli_saddr));
    return snprintf(b, n, "%s+%#lx", f, (unsigned long)(a - (uint64_t)di.dli_fbase));
}
// A return address without its pointer authentication code: Android's libraries sign the return addresses they keep on the stack (PAC, in the bits above the virtual
// address size), so a saved one read raw (0x68c07e50424a5c) resolved to nothing. The address size is taken from where this library is mapped, near the top of the
// user range (39 bits on the Y700; not from a local: the crash handler runs on an alternate stack from malloc, whose pointers carry a tag byte). Only for return
// addresses: a heap pointer's top byte is a tag the register dump keeps
uint64_t crash_code_addr(uint64_t a) {
    uint64_t ref = (uint64_t)(uintptr_t)crash_code_addr, mask = (1ull << (64 - __builtin_clzll(ref))) - 1;
    return a > mask && (a & mask) >= 0x100000000ull ? a & mask : a;
}
// A thread's name by its thread port (the tid on Android), for the crash and thread dumps: "vk prewarm", "vk async pipe" or the replay tells which compile crashed (issue #64)
void crash_thread_name(unsigned t, char *b, size_t n) {
    b[0] = 0;
#ifdef __ANDROID__
    char path[64]; snprintf(path, sizeof path, "/proc/self/task/%u/comm", (unsigned)t);
    int fd = open(path, O_RDONLY); if (fd < 0) return;
    ssize_t r = read(fd, b, n - 1); close(fd); if (r <= 0) { b[0] = 0; return; }
    b[r] = 0; if (b[r - 1] == '\n') b[r - 1] = 0;
#else
    pthread_t p = pthread_from_mach_thread_np(t); if (p) pthread_getname_np(p, b, n);
#endif
}
void crash_off(const char *tag, uint64_t a) {   // Also used by diag.c
#define off crash_off

    char b[192]; crash_addr_str(b, sizeof b, a);
    fprintf(stderr, " %s%s%s", tag, *tag ? "=" : "", b);
}
// Frame pointer backtrace: x29 chain [fp]=previous frame's fp, [fp+8]=return address; only 8-byte aligned, monotonically increasing frames are walked.
// Each frame is read with vm_read_overwrite, which fails instead of faulting: code built without frame pointers keeps data in x29 (STEINS;GATE RE:BOOT's main+0x38921c: x29 = 0x430),
// and a probe's backtrace there used to crash the host
void dm_backtrace(uint64_t fp) {
    fprintf(stderr, "[dm] bt:");
    for (int i = 0; i < 32 && fp && !(fp & 7); i++) {
        uint64_t w[2]; vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof w, (vm_address_t)w, &got) != KERN_SUCCESS || got != sizeof w) break;
        uint64_t nfp = w[0], ra = w[1];
        off("", crash_code_addr(ra));
        if (nfp <= fp) break;
        fp = nfp;
    }
    fputc('\n', stderr);
}
// ---------- Page fault hooks (write-protect dirty page tracking) ----------
// Called in registration order before on_crash prints the crash state; returning 1 = handled (return directly and re-execute the faulting instruction); the crash path is taken only when all return 0.
// Hooks run in signal handler context: only table lookups / atomic operations / mprotect are allowed, no memory allocation and no locking.
#define MAX_FAULT_HOOK 4
static int (*g_fault_hook[MAX_FAULT_HOOK])(void *addr, int is_write, void *uc);
static void (*g_write_prep[MAX_FAULT_HOOK])(void *addr, size_t len);
static int g_nfault_hook, g_nwrite_prep;
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) {
    if (g_nfault_hook >= MAX_FAULT_HOOK) die("too many page fault hooks");
    g_fault_hook[g_nfault_hook++] = fn;
}
static void (*g_res_report)(uint32_t tick, const char *why);   // Resource count callback (registered by the graphics layer, called by diag.c per tick / event)
void dm_res_report_register(void (*fn)(uint32_t tick, const char *why)) { g_res_report = fn; }
void (*dm_res_report_cb(void))(uint32_t, const char *) { return g_res_report; }
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) {
    if (g_nwrite_prep >= MAX_FAULT_HOOK) die("too many host pre-write hooks");
    g_write_prep[g_nwrite_prep++] = fn;
}
// Called before the host itself writes guest memory (read() straight into a guest buffer, etc.): for a system call a write-protected page does not trigger a page fault but returns EFAULT
void dm_guest_write_prepare(void *addr, size_t len) { for (int i = 0; i < g_nwrite_prep; i++) g_write_prep[i](addr, len); }

// The diagnostics module (the probes in diag.c) and the thread state dump (hle_thread.c) each register themselves here: a unit test that links only this file does not depend on them
static int (*g_trap_hook)(ucontext_t *uc);
void crash_set_trap_hook(int (*fn)(ucontext_t *uc)) { g_trap_hook = fn; }
void crash_set_dump_hook(void (*fn)(void)) { g_dump_hook = fn; }
void crash_set_wait_dump(void (*fn)(void)) { g_wait_dump = fn; }
// Notes printed right after the crashing thread's registers: what host threads were doing when it crashed (the Vulkan backend: the pipelines being built, gfx_vk_exec.c)
static void (*g_note_hook[4])(void); static int g_nnote_hook;
void crash_add_note_hook(void (*fn)(void)) { if (g_nnote_hook < 4) g_note_hook[g_nnote_hook++] = fn; }
static struct sigaction g_old_sa[32]; static int g_old_ok[32];   // the handlers before ours (DM_CRASH_TOMBSTONE)
// Dump the current state on demand (without exiting): the iOS app has no no-progress watchdog, so when the smoke test fails it sends dump through DM_LOG_PORT to trigger this -- what each guest thread is waiting on + the graphics-side wait points, the same thing the watchdog prints
void crash_dump_now(const char *why) { fprintf(stderr, "[dm] state dump (%s): tick %u / stage %s\n", why, atomic_load(&g_watch_tick), g_watch_stage[atomic_load(&g_watch_stage_idx)]); if (g_dump_hook) g_dump_hook(); if (g_wait_dump) g_wait_dump(); }
// Only the first crashing thread prints its state: when several threads crash at once (Unity's job threads often do), the dumps used to interleave and the process hung in the dump without exiting.
// Later threads stop in pause() until the process exits; the first thread re-entering (crashing again while printing, or the SIGALRM of the 15 s safety below) exits at once.
static _Atomic uintptr_t g_crash_owner;
static void crash_gate(int sig) {
    uintptr_t me = (uintptr_t)pthread_self(), none = 0;
    if (atomic_compare_exchange_strong(&g_crash_owner, &none, me)) { alarm(15); return; }
    if (none == me) _exit(128 + sig);
    for (;;) pause();
}
static void on_crash(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    if (sig == SIGTRAP && g_trap_hook && g_trap_hook(uc)) return;
    if (sig == SIGSEGV || sig == SIGBUS) {
        int is_write = (int)((DM_UC_ESR(uc) >> 6) & 1);   // The WnR bit of ESR
        for (int i = 0; i < g_nfault_hook; i++) if (g_fault_hook[i](si->si_addr, is_write, uc)) return;
    }
    crash_gate(sig);
    // The stderr lock may be held by another thread (stuck or suspended): the fprintf below would hang with it, the time limit's SIGALRM could not end the run, and the log would get nothing.
    // If it still cannot be taken after 1 s, write a single line with write() (pc / lr as module offsets) and exit. When this thread itself holds the lock, ftrylockfile still succeeds (file locks are recursive), unaffected
    for (int i = 0; ftrylockfile(stderr); i++) {
        if (i < 100) { usleep(10000); continue; }
        char b[256]; int n = snprintf(b, sizeof b, "[dm] CRASH sig=%d (stderr held for 1 s, no dump)", sig);
        uint64_t at[2] = {DM_UC_SS(uc)->__pc, DM_UC_SS(uc)->__lr};
        for (int k = 0; k < 2; k++) { module_t *m = module_of((void *)at[k]);
            n += m ? snprintf(b + n, sizeof b - n, " %s=%s+%#llx", k ? "lr" : "pc", m->name, at[k] - (uint64_t)m->base) : snprintf(b + n, sizeof b - n, " %s=%#llx", k ? "lr" : "pc", at[k]); }
        n += snprintf(b + n, sizeof b - n, "\n");
        if (write(2, b, (size_t)n) < 0) {}
        _exit(128 + sig);
    }
    funlockfile(stderr);
    fprintf(stderr, "[dm] CRASH sig=%d", sig);
    off("addr", (uint64_t)si->si_addr); off("pc", DM_UC_SS(uc)->__pc); off("lr", crash_code_addr(DM_UC_SS(uc)->__lr));
    { mach_port_t me = mach_thread_self(); char nm[64]; crash_thread_name(me, nm, sizeof nm);
      fprintf(stderr, " sp=%#llx thread %#x '%s'\n", DM_UC_SS(uc)->__sp, (unsigned)me, nm); }
    for (int i = 0; i < 29; i++) fprintf(stderr, "x%d=%#llx%s", i, DM_UC_SS(uc)->__x[i], i % 6 == 5 ? "\n" : " ");
    fprintf(stderr, "\n");
    for (int i = 0; i < g_nnote_hook; i++) g_note_hook[i]();   // (before the long dumps below: the most useful lines come first)
    // What the registers point at: the first six words of each readable target (read with vm_read_overwrite, which fails instead of faulting); a word inside a module prints as a module offset, so an object's vtable names its class
    for (int i = 0; i < 29; i++) {
        uint64_t a = DM_UC_SS(uc)->__x[i], w[6]; vm_size_t got = 0;
        if (a < 0x100000000ull || (a & 7) || vm_read_overwrite(mach_task_self(), (vm_address_t)a, sizeof w, (vm_address_t)w, &got) != KERN_SUCCESS || got != sizeof w) continue;
        fprintf(stderr, "[dm] *x%d:", i);
        for (int k = 0; k < 6; k++) off("", w[k]);
        fputc('\n', stderr);
    }
    dm_backtrace(DM_UC_SS(uc)->__fp);
    // Stack scan: when the frame chain is broken (a return address was overwritten, pc = lr lands on garbage) the backtrace prints nothing; scan 512 words up from sp and list everything that resolves to a module / host symbol (nearest callers first)
    {   uint64_t sp = DM_UC_SS(uc)->__sp;   // The frame just popped (when a return address was overwritten, the bad value and the leftovers of whatever overwrote it sit below sp)
        fprintf(stderr, "[dm] fp=%#llx  0x100 bytes below sp:", (unsigned long long)DM_UC_SS(uc)->__fp);
        for (int i = -32; i < 0 && sp && !(sp & 7); i++) fprintf(stderr, "%s%llx", (i + 32) % 4 ? " " : "\n[dm]   ", (unsigned long long)((uint64_t *)sp)[i]);
        fputc('\n', stderr); }
    // Other libraries' addresses too (library+offset): the Adreno driver keeps no frame pointer, so a crash inside it (issue #64) has no backtrace, and the return addresses
    // into the driver and libvulkan above sp are the only callers left. Data pointers into a library's segments show the same way; only text addresses are callers
    {   uint64_t sp = DM_UC_SS(uc)->__sp; int n = 0;
        fprintf(stderr, "[dm] stack scan sp=%#llx:", (unsigned long long)sp);
        for (int i = -32; i < 512 && n < 40 && sp && !(sp & 7); i++) {
            uint64_t v = crash_code_addr(((uint64_t *)sp)[i]); Dl_info di; char b[192];
            if (v < 0x100000000ull || (v & 3)) continue;
            if (!module_of((void *)v) && !(dladdr((void *)v, &di) && di.dli_fname)) continue;
            crash_addr_str(b, sizeof b, v); fprintf(stderr, " [%+d]%s", i * 8, b); n++;
        }
        fputc('\n', stderr); }
    if (g_dump_hook) g_dump_hook();
    // DM_CRASH_TOMBSTONE=1: after this dump, hand the signal to the handler that was there before ours (Android's debuggerd), which writes a tombstone with the
    // system's unwinder (it reads the libraries' unwind tables, so it walks through a driver that keeps no frame pointer). A fault returns and runs the instruction
    // again, now under that handler; a raised signal (abort) is raised again for it
    if (sig != SIGALRM && g_old_ok[sig & 31] && getenv("DM_CRASH_TOMBSTONE") && atoi(getenv("DM_CRASH_TOMBSTONE"))) {
        fprintf(stderr, "[dm] CRASH: the signal goes on to the system's handler (DM_CRASH_TOMBSTONE): see the tombstone\n"); fflush(stderr);
        alarm(0); sigaction(sig, &g_old_sa[sig & 31], NULL);
        if (si->si_code <= 0) raise(sig);   // (sent by a thread: blocked until this handler returns)
        return;
    }
    _exit(128 + sig);
}
// The signal alternate stack is per thread: every guest thread must call this when it starts running (only then can the crash state be printed on the alternate stack on a stack overflow)
// The alternate stack is released when the thread exits: the pthread key destructor is called at thread exit (disable the alternate stack first, then free). The game frequently starts short-lived threads, and without releasing each one leaks 64 KiB.
static pthread_key_t g_altstack_key;
static pthread_once_t g_altstack_once = PTHREAD_ONCE_INIT;
static void altstack_free(void *p) { stack_t off = {.ss_flags = SS_DISABLE}; sigaltstack(&off, 0); free(p); }
static void altstack_key_init(void) { if (pthread_key_create(&g_altstack_key, altstack_free)) die("altstack key"); }
void crash_thread_init(void) {
    pthread_once(&g_altstack_once, altstack_key_init);
    if (pthread_getspecific(g_altstack_key)) return;                 // Repeated calls on the same thread do not allocate again
    stack_t ss = {.ss_sp = malloc(65536), .ss_size = 65536}; sigaltstack(&ss, 0);
    pthread_setspecific(g_altstack_key, ss.ss_sp);
}
void crash_install(void) {
    crash_thread_init();
    struct sigaction sa = {.sa_sigaction = on_crash, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigaction(SIGALRM, &sa, 0);
    // Both watchdogs are turned on only when a test explicitly sets DM_WATCH / DM_WATCH_STALL: the product (the player launches from the desktop, no environment variables) turns on neither.
    // Found by reading the code: iOS main.m did setenv DM_WATCH=30 by default → the player's launch was killed by SIGALRM at 30 s; the no-progress watchdog defaulted to 15 s → the pause menu (ticks stopped) was killed at 15 s
    const char *watch = getenv("DM_WATCH"), *stall = getenv("DM_WATCH_STALL");
    if (watch) alarm(atoi(watch));
    if (watch || stall) {
        g_watch_stall = stall && atoi(stall) > 0 ? (unsigned)atoi(stall) : 15;
        atomic_store(&g_watch_last_ns, watch_now_ns());
        pthread_t th; if (pthread_create(&th, NULL, watchdog_main, NULL)) die("watchdog thread");
        pthread_detach(th);
    }
    // abort() too (an assertion or an uncaught exception in the host or a framework): Big Brain Academy: Brain vs. Brain died once with SIGABRT 11 minutes into a run with nothing
    // in the log and no crash report, so where it came from was lost
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT};
    for (int i = 0; i < 5; i++) { struct sigaction o; sigaction(sigs[i], &sa, &o);
        if (!(o.sa_flags & SA_SIGINFO && o.sa_sigaction == on_crash)) { g_old_sa[sigs[i]] = o; g_old_ok[sigs[i]] = 1; } }   // (a second call keeps the first's)
}
