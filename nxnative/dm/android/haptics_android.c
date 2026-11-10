// Android port: vibration. The game's rumble (hle_haptics.c's state for player 1) and the touch gamepad's taps go to android.os.Vibrator through JNI, on a
// thread of its own: the haptics sink runs on the game's thread under hle_haptics.c's lock, and every vibrate call is a binder transaction. Rumble goes to the
// controller in use when it has a motor (InputDevice.getVibrator), else to the device; taps always go to the device. The player's Rumble setting turns both off.
// Android has no continuous rumble with a changing strength, so a strength is a one-shot of RUMBLE_MS renewed while the game keeps the motor running.
// The strength follows an envelope (BananaNative's Rumble.java): every TICK_NS the strongest level the game set since the last tick, not the latest one, and once the
// game goes quiet a fall of FALL a tick (about 0.2 s from full to off). Switch 2 games stream HD rumble (Donkey Kong Bananza: 400 values a second, short peaks with
// near-silence between); taking the latest value mostly lands on the silence or cuts each buzz short, so the motor never spins up.
#include "dm.h"
#include "hle_haptics.h"
#include <jni.h>
#include <android/native_activity.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RUMBLE_MS 600
#define RENEW_NS 400000000ll
#define TICK_NS 40000000ll   // the envelope's step
#define FALL 0.6f            // the envelope's fall per tick once the game is quiet
#define MOVE 6               // a new vibration only when the strength moved this much (of 255)
static JavaVM *g_vm; static jobject g_actobj;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static dm_haptic_state g_state; static float g_peak; static int g_taps, g_dev = -1, g_dev_new; static _Atomic int g_on = 1;
static int64_t mono_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
static void sink(dm_haptic_state s) {   // hle_haptics.c, under its lock: only hand the state over
    if (s.player) return;
    float lv = dm_haptics_level(s.motor, 2, 1.0f).intensity;
    pthread_mutex_lock(&g_mu); g_state = s; if (lv > g_peak) { if (g_peak <= 0) pthread_cond_signal(&g_cv); g_peak = lv; } pthread_mutex_unlock(&g_mu);   // a wake only to start ticking
}
void dm_haptics_android_tap(void) { if (!atomic_load(&g_on) || !g_vm) return; pthread_mutex_lock(&g_mu); g_taps++; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu); }
void dm_haptics_android_device(int id) {   // the input device in use: a controller's id, -1 the touch screen
    pthread_mutex_lock(&g_mu); if (id != g_dev) { g_dev = id; g_dev_new = 1; pthread_cond_signal(&g_cv); } pthread_mutex_unlock(&g_mu);
}
void dm_haptics_android_set(int on) { atomic_store(&g_on, on != 0); pthread_mutex_lock(&g_mu); pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu); }

static JNIEnv *E;
static jclass cVE, cID; static jmethodID mOneShot, mPredef, mVibrate, mCancel, mHas, mAmp, mGetDevice, mGetVib;
typedef struct { jobject v; int amp; } vib_t;   // a Vibrator (global reference) and whether it controls amplitude
static int jok(const char *what) {
    if (!(*E)->ExceptionCheck(E)) return 1;
    (*E)->ExceptionClear(E); LOG("[RED] [haptics] Java exception in %s", what); return 0;
}
static int vib_take(vib_t *v, jobject o) {   // o: a local reference, released here
    v->v = NULL; v->amp = 0;
    if (!o) return 0;
    jboolean has = (*E)->CallBooleanMethod(E, o, mHas); if (!jok("hasVibrator")) has = 0;
    if (has) { v->v = (*E)->NewGlobalRef(E, o); v->amp = (*E)->CallBooleanMethod(E, o, mAmp); if (!jok("hasAmplitudeControl")) v->amp = 0; }
    (*E)->DeleteLocalRef(E, o);
    return v->v != NULL;
}
static void vib_drop(vib_t *v) { if (v->v) (*E)->DeleteGlobalRef(E, v->v); v->v = NULL; }
static void vib_effect(vib_t *v, jobject e) { if (!e) return; (*E)->CallVoidMethod(E, v->v, mVibrate, e); jok("vibrate"); (*E)->DeleteLocalRef(E, e); }
static void vib_play(vib_t *v, int ms, int amp) { jobject e = (*E)->CallStaticObjectMethod(E, cVE, mOneShot, (jlong)ms, (jint)(v->amp ? amp : -1)); if (jok("createOneShot")) vib_effect(v, e); }
static void vib_tap(vib_t *v) {   // a short, light tick; without amplitude control the system's click
    if (v->amp) { vib_play(v, 14, 140); return; }
    jobject e = (*E)->CallStaticObjectMethod(E, cVE, mPredef, (jint)0 /* EFFECT_CLICK */); if (jok("createPredefined")) vib_effect(v, e);
}
static void vib_stop(vib_t *v) { (*E)->CallVoidMethod(E, v->v, mCancel); jok("cancel"); }

static void *haptics_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "haptics");
    if ((*g_vm)->AttachCurrentThread(g_vm, &E, NULL) != JNI_OK) { LOG("[RED] [haptics] cannot attach to the Java VM"); return NULL; }
    jclass cCtx = (*E)->FindClass(E, "android/content/Context"), cVib = (*E)->FindClass(E, "android/os/Vibrator");
    cVE = (*E)->NewGlobalRef(E, (*E)->FindClass(E, "android/os/VibrationEffect")); cID = (*E)->NewGlobalRef(E, (*E)->FindClass(E, "android/view/InputDevice"));
    if (!jok("FindClass") || !cCtx || !cVib || !cVE || !cID) { LOG("[RED] [haptics] Vibrator classes missing"); return NULL; }
    jmethodID mSvc = (*E)->GetMethodID(E, cCtx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    mOneShot = (*E)->GetStaticMethodID(E, cVE, "createOneShot", "(JI)Landroid/os/VibrationEffect;");
    mPredef = (*E)->GetStaticMethodID(E, cVE, "createPredefined", "(I)Landroid/os/VibrationEffect;");
    mVibrate = (*E)->GetMethodID(E, cVib, "vibrate", "(Landroid/os/VibrationEffect;)V"); mCancel = (*E)->GetMethodID(E, cVib, "cancel", "()V");
    mHas = (*E)->GetMethodID(E, cVib, "hasVibrator", "()Z"); mAmp = (*E)->GetMethodID(E, cVib, "hasAmplitudeControl", "()Z");
    mGetDevice = (*E)->GetStaticMethodID(E, cID, "getDevice", "(I)Landroid/view/InputDevice;"); mGetVib = (*E)->GetMethodID(E, cID, "getVibrator", "()Landroid/os/Vibrator;");
    if (!jok("GetMethodID")) { LOG("[RED] [haptics] Vibrator methods missing"); return NULL; }
    jstring name = (*E)->NewStringUTF(E, "vibrator");
    vib_t dev, pad = {0}; vib_take(&dev, (*E)->CallObjectMethod(E, g_actobj, mSvc, name)); jok("getSystemService"); (*E)->DeleteLocalRef(E, name);
    LOG("[haptics] device vibrator: %s%s", dev.v ? "yes" : "none", dev.v ? dev.amp ? ", with amplitude control" : ", on/off only" : "");
    dm_haptics_route(0, 1);   // rumble has somewhere to go from now on (a controller's motor, or the device's)
    vib_t *cur = NULL; int cur_amp = 0, was_on = 1; int64_t last = 0, next = 0; float env = 0;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        for (;;) {   // asleep until a tap, a device change, the setting, or rumble to play; while the envelope runs, a tick every TICK_NS
            int64_t t = mono_now(); int on = atomic_load(&g_on);
            if (g_taps || g_dev_new || on != was_on) break;
            if (!env && !cur_amp) { if (g_peak > 0) { next = t; break; } pthread_cond_wait(&g_cv, &g_mu); continue; }
            if (t >= next) break;
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); int64_t w = next - t; dl.tv_sec += w / 1000000000; dl.tv_nsec += w % 1000000000;
            if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_cv, &g_mu, &dl);
        }
        int taps = g_taps, dn = g_dev_new, id = g_dev; dm_haptic_state s = g_state; float peak = g_peak; int64_t t = mono_now(), tick = t >= next;
        g_taps = 0; g_dev_new = 0; if (tick) g_peak = 0;
        pthread_mutex_unlock(&g_mu);
        was_on = atomic_load(&g_on);
        if (dn) {
            if (cur == &pad && cur_amp) { vib_stop(&pad); cur_amp = 0; }
            vib_drop(&pad);
            if (id >= 0) { jobject d = (*E)->CallStaticObjectMethod(E, cID, mGetDevice, (jint)id); if (jok("getDevice") && d) { jobject v = (*E)->CallObjectMethod(E, d, mGetVib); if (jok("getVibrator")) vib_take(&pad, v); (*E)->DeleteLocalRef(E, d); } }
            LOG("[haptics] input device %d: rumble on %s", id, pad.v ? pad.amp ? "the controller (amplitude control)" : "the controller (on/off)" : dev.v ? "the device" : "nothing");
        }
        if (taps && dev.v && was_on) { vib_tap(&dev); if (cur == &dev && cur_amp) last = t - RENEW_NS; }   // a running rumble comes back at the next tick
        if (!tick && !dn && !(!was_on && cur_amp)) continue;
        if (tick) {   // the envelope: the strongest level since the last tick, or the fall
            float now = dm_haptics_level(s.motor, 2, 1.0f).intensity, want = peak > now ? peak : now;
            env = want >= env ? want : env * FALL > want ? env * FALL : want;
            if (env < 4 / 255.0f) env = 0;
            next = t + TICK_NS;
        }
        vib_t *to = pad.v ? &pad : dev.v ? &dev : NULL;
        int amp = was_on && to ? (int)(env * 255 + 0.5f) : 0;
        if (to && !to->amp && amp) amp = amp >= 64 ? 255 : 0;   // on/off motors: a strong enough rumble only
        if (amp < 10) { if (cur_amp) { vib_stop(cur); cur_amp = 0; } continue; }
        if (to != cur || abs(amp - cur_amp) >= MOVE || t - last >= RENEW_NS) {
            static int first = 1; if (first) { first = 0; LOG("[haptics] first rumble from the game: strength %d/255", amp); }
            if (cur && cur != to && cur_amp) vib_stop(cur);
            vib_play(to, RUMBLE_MS, amp); cur = to; cur_amp = amp; last = t;
        }
    }
    return NULL;
}
void dm_haptics_android_start(ANativeActivity *act, int on) {
    g_vm = act->vm; g_actobj = act->clazz;   // the NativeActivity object: a global reference the framework keeps
    atomic_store(&g_on, on != 0);
    dm_haptics_init(sink, 1);
    pthread_t t; if (!pthread_create(&t, NULL, haptics_thread, NULL)) pthread_detach(t);
}
