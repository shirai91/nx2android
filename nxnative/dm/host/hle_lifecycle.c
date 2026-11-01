// Host lifecycle: graceful exit (SIGTERM; on iOS the App termination callback calls dm_host_shutdown).
// Pause audio, suspend graphics (wait for in-flight NVN calls to end and drain GPU work), then _exit(0); a 1.5 s fallback goes straight to _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is stopping background threads in order, exit code 0, and giving the iOS lifecycle callback an entry.
#include "hle.h"
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <time.h>
#endif
void hlecount_flush(void); void dm_rec_finish(void);
#if !defined(__APPLE__)
static void *exit_fallback(void *a) {   // Android port: the 1.5 s fallback on a thread of its own (no GCD)
    (void)a; struct timespec t = {1, 500000000}; while (nanosleep(&t, &t)) ;
    LOG("[RED] graceful exit not done in 1.5 seconds, exiting at once"); _exit(0);
}
#endif
void dm_host_shutdown(const char *why) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    uint64_t t0 = now_ns(); LOG("received %s: graceful exit", why);
#if defined(__APPLE__)
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        LOG("[RED] graceful exit not done in 1.5 seconds, exiting at once"); _exit(0); });
#else
    pthread_t th; if (!pthread_create(&th, NULL, exit_fallback, NULL)) pthread_detach(th);
#endif
    hlecount_flush();                               // DM_HLECOUNT: _exit does not run atexit, write a copy here
    dm_rec_finish();                                // DM_REC: the held state and the buffered steps (hle_hid.c)
    dm_cheats_exit();                               // a clean exit: no safe mode for the cheats at the next launch (atexit does not run either)
    dm_audio_suspend(1);
    dm_gfx_suspend(1);
    LOG("graceful exit done (%.3f s)", (now_ns() - t0) / 1e9);
    _exit(0);
}
#if !defined(__APPLE__)
static void *term_wait(void *a) {   // Android port: SIGTERM is blocked in every thread created after dm_term_install and taken here with sigwait
    sigset_t *s = a; int sig;
    for (;;) if (!sigwait(s, &sig) && sig == SIGTERM) dm_host_shutdown("SIGTERM");
}
#endif
void dm_term_install(void) {
#if defined(__APPLE__)
    signal(SIGTERM, SIG_IGN);                       // do not take the default action: hand over to the GCD signal source to wrap up on an ordinary thread
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ dm_host_shutdown("SIGTERM"); });
    dispatch_resume(src);
#else
    static sigset_t s; sigemptyset(&s); sigaddset(&s, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &s, NULL);           // inherited by the threads created from here on (the guest's among them)
    pthread_t th; if (!pthread_create(&th, NULL, term_wait, &s)) pthread_detach(th);
#endif
}
