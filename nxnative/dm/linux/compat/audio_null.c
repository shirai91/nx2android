// Linux port: an AudioUnit output unit with no device yet (dm/linux/compat/include/AudioToolbox/AudioToolbox.h).
// A thread pulls the host's render callback at the stream's real-time pace and drops the samples, so the game's mixer runs as it would with a sound card.
// The real sink (SDL2 audio or PipeWire) replaces dm_sink_write.
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct dm_audio_component { int unused; };
struct dm_audio_unit {
    AudioStreamBasicDescription fmt; int have_fmt;
    AURenderCallbackStruct cb;
    int32_t frames_hint;
    pthread_t th; int th_live;
    _Atomic int run;      // the thread keeps pulling
    _Atomic int held;     // stopped: silence, the host's output is not touched
    _Atomic int in_cb;
    _Atomic uint64_t sample_time;
};
static struct dm_audio_component g_output;

// Where finished audio goes. Null: nowhere.
static void dm_sink_write(AudioUnit u, const void *pcm, uint32_t frames) { (void)u; (void)pcm; (void)frames; }

AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d) {
    return !after && d && d->componentType == kAudioUnitType_Output ? &g_output : NULL;
}
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out) {
    if (c != &g_output) return kAudio_UnimplementedError;
    *out = calloc(1, sizeof **out); return *out ? noErr : -1;
}
OSStatus AudioUnitSetProperty(AudioUnit u, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el, const void *data, UInt32 size) {
    (void)scope; (void)el;
    switch (id) {
    case kAudioUnitProperty_StreamFormat:
        if (size < sizeof u->fmt) return -50;
        memcpy(&u->fmt, data, sizeof u->fmt);
        if (u->fmt.mFormatID != kAudioFormatLinearPCM || (u->fmt.mFormatFlags & kAudioFormatFlagIsNonInterleaved)) return kAudioUnitErr_FormatNotSupported;
        if (!(u->fmt.mFormatFlags & kAudioFormatFlagIsFloat) && u->fmt.mBitsPerChannel != 16) return kAudioUnitErr_FormatNotSupported;
        u->have_fmt = 1; return noErr;
    case kAudioUnitProperty_SetRenderCallback: if (size < sizeof u->cb) return -50; memcpy(&u->cb, data, sizeof u->cb); return noErr;
    case kAudioDevicePropertyBufferFrameSize: if (size >= 4) u->frames_hint = (int32_t)*(const UInt32 *)data; return noErr;
    default: return noErr;
    }
}
OSStatus AudioUnitInitialize(AudioUnit u) { return u->have_fmt && u->cb.inputProc ? noErr : -50; }

static void *pull(void *arg) {
    AudioUnit u = arg;
    uint32_t frames = u->frames_hint > 0 ? (uint32_t)u->frames_hint : (uint32_t)(u->fmt.mSampleRate / 200);
    size_t bytes = (size_t)frames * u->fmt.mBytesPerFrame;
    void *buf = calloc(1, bytes);
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    while (atomic_load(&u->run)) {
        atomic_fetch_add(&u->in_cb, 1);
        if (atomic_load(&u->held)) { memset(buf, 0, bytes); atomic_fetch_add(&u->sample_time, frames); }
        else {
            AudioBufferList bl = {1, {{u->fmt.mChannelsPerFrame, (UInt32)bytes, buf}}};
            AudioUnitRenderActionFlags fl = 0;
            struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
            AudioTimeStamp ts = {.mSampleTime = (Float64)atomic_fetch_add(&u->sample_time, frames), .mHostTime = (UInt64)t.tv_sec * 1000000000ull + (UInt64)t.tv_nsec};
            if (u->cb.inputProc(u->cb.inputProcRefCon, &fl, &ts, 0, frames, &bl)) memset(buf, 0, bytes);
            dm_sink_write(u, buf, frames);
        }
        atomic_fetch_sub(&u->in_cb, 1);
        long ns = (long)((double)frames / u->fmt.mSampleRate * 1e9);
        next.tv_nsec += ns; while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    free(buf); return NULL;
}
static void end_thread(AudioUnit u) {
    if (!u->th_live) return;
    atomic_store(&u->run, 0); pthread_join(u->th, NULL); u->th_live = 0;
}
OSStatus AudioOutputUnitStart(AudioUnit u) {
    if (u->th_live) { atomic_store(&u->held, 0); return noErr; }
    atomic_store(&u->held, 0); atomic_store(&u->run, 1);
    if (pthread_create(&u->th, NULL, pull, u)) return -1;
    u->th_live = 1; return noErr;
}
// Once this returns no callback is inside the host's render callback and none enters it until the next start
OSStatus AudioOutputUnitStop(AudioUnit u) {
    if (!u->th_live) return noErr;
    atomic_store(&u->held, 1);
    while (atomic_load(&u->in_cb)) sched_yield();
    return noErr;
}
OSStatus AudioUnitUninitialize(AudioUnit u) { end_thread(u); return noErr; }
OSStatus AudioComponentInstanceDispose(AudioComponentInstance u) { if (u) { end_thread(u); free(u); } return noErr; }

// AudioConverter (AAC to PCM) is not available yet: the movie library falls back to silent movies
OSStatus AudioConverterNew(const AudioStreamBasicDescription *in, const AudioStreamBasicDescription *out, AudioConverterRef *c) { (void)in; (void)out; (void)c; return kAudio_UnimplementedError; }
OSStatus AudioConverterSetProperty(AudioConverterRef c, AudioConverterPropertyID id, UInt32 size, const void *data) { (void)c; (void)id; (void)size; (void)data; return kAudio_UnimplementedError; }
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef c, AudioConverterComplexInputDataProc proc, void *ref, UInt32 *packets, AudioBufferList *out, AudioStreamPacketDescription *desc) { (void)c; (void)proc; (void)ref; (void)packets; (void)out; (void)desc; return kAudio_UnimplementedError; }
OSStatus AudioConverterDispose(AudioConverterRef c) { (void)c; return noErr; }
