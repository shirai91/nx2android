// Android port: an AudioUnit output unit on AAudio (dm/android/compat/include/AudioToolbox/AudioToolbox.h).
// The stream asks for low latency and the unit's own format (interleaved 16-bit or float PCM at its rate and channel count); AAudio's data callback
// calls the unit's render callback with one AudioBuffer over AAudio's buffer, so the host's pull code (hle_audio.c pull_device) is unchanged.
// A stream that disconnects (headphones unplugged, another app taking exclusive audio) stops calling back; the host's watchdog then retries the start,
// which opens a new stream.
#include <AudioToolbox/AudioToolbox.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "audio_route_policy.h"

struct dm_audio_component { int unused; };
struct dm_audio_unit {
    AudioStreamBasicDescription fmt; int have_fmt;
    AURenderCallbackStruct cb;
    int32_t frames_hint;
    AAudioStream *st;
    int32_t requested_device;
    _Atomic uint64_t sample_time;
    uint64_t stop_time;    // sample_time at the unit's last stop: unchanged at the next start = the stream stopped calling back (a stall), so it is opened again
    _Atomic int failed;    // the error callback came (disconnected): the next start opens a new stream
    _Atomic int held, in_cb;   // held: stopped, the stream calls back with silence and never into the host (AudioOutputUnitStop); in_cb: data callbacks running
    uint64_t held_frames;  // silence played since the stop: past HOLD_MAX the stream stops itself
};
static struct dm_audio_component g_output;
static _Atomic int32_t g_output_device;
void dm_aaudio_set_device(int32_t id) { atomic_store(&g_output_device, id); }
int32_t dm_aaudio_get_device(void) { return atomic_load(&g_output_device); }
int dm_aaudio_route_stale(AudioUnit u) { return u && u->requested_device != atomic_load(&g_output_device); }

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

enum { HOLD_MAX_S = 60 };   // a stop longer than this (the app in the background) ends the stream: the next start opens a new one
static aaudio_data_callback_result_t on_data(AAudioStream *st, void *ud, void *audio, int32_t frames) {
    AudioUnit u = ud;
    atomic_fetch_add(&u->in_cb, 1);
    if (atomic_load(&u->held)) {   // stopped: silence, and the host's output (which it may have freed) is not touched
        memset(audio, 0, (size_t)frames * u->fmt.mBytesPerFrame); atomic_fetch_add(&u->sample_time, (uint64_t)frames);
        int end = (u->held_frames += (uint64_t)frames) > (uint64_t)(u->fmt.mSampleRate * HOLD_MAX_S);
        atomic_fetch_sub(&u->in_cb, 1); (void)st; return end ? AAUDIO_CALLBACK_RESULT_STOP : AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    AudioBufferList bl = {1, {{u->fmt.mChannelsPerFrame, (UInt32)frames * u->fmt.mBytesPerFrame, audio}}};
    AudioUnitRenderActionFlags fl = 0;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    AudioTimeStamp ts = {.mSampleTime = (Float64)atomic_fetch_add(&u->sample_time, (uint64_t)frames), .mHostTime = (UInt64)t.tv_sec * 1000000000ull + (UInt64)t.tv_nsec};
    if (u->cb.inputProc(u->cb.inputProcRefCon, &fl, &ts, 0, (UInt32)frames, &bl)) memset(audio, 0, (size_t)frames * u->fmt.mBytesPerFrame);
    atomic_fetch_sub(&u->in_cb, 1); (void)st; return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void on_error(AAudioStream *st, void *ud, aaudio_result_t err) {
    (void)st; AudioUnit u = ud; atomic_store(&u->failed, 1);
    if (err == AAUDIO_ERROR_DISCONNECTED && audio_route_fallback(&g_output_device, u->requested_device))
        __android_log_print(ANDROID_LOG_WARN, "dmhost", "AAudio selected device disconnected; falling back to system output");
    __android_log_print(ANDROID_LOG_WARN, "dmhost", "AAudio stream error %d (%s): the host's watchdog restarts it", err, AAudio_convertResultToText(err));
}
// requestStop only asks: the data callback may still be running after it returns, and the host frees the output it pulls from once the unit is stopped
// (hle_audio.c au_close; games close and open outputs when the operation mode changes). So ending a stream (uninitialize, dispose, opening a new one) waits
// until it is stopped, and a stream that does not get there within a second is closed
static void close_stream(AudioUnit u) { if (u->st) { AAudioStream_close(u->st); u->st = NULL; } }
static void stop_stream(AudioUnit u) {
    if (!u->st) return;
    aaudio_stream_state_t s = AAUDIO_STREAM_STATE_UNKNOWN;
    if (AAudioStream_requestStop(u->st) == AAUDIO_OK) {
        s = AAudioStream_getState(u->st);
        for (int i = 0; i < 10 && s != AAUDIO_STREAM_STATE_STOPPED && s != AAUDIO_STREAM_STATE_DISCONNECTED && s != AAUDIO_STREAM_STATE_CLOSED; i++)
            if (AAudioStream_waitForStateChange(u->st, s, &s, 100000000) != AAUDIO_OK) break;
    }
    if (s != AAUDIO_STREAM_STATE_STOPPED) close_stream(u);
}

OSStatus AudioOutputUnitStart(AudioUnit u) {
    // A stop and start (the pause menu holds the game, the app goes to the background) go on with the same stream, which played silence meanwhile. Each new
    // stream is a new AudioFlinger track, and on the Legion Y700 the app's first was a fast track (bypassing the output's Dolby DAP effect), the ones after it
    // normal mixer tracks through it: the sound got louder after the pause menu (13 Sentinels, four toggles: track 1536 frames fast, then four of 2886; the
    // game's own samples at -30 dBFS throughout). Nor is the stream stopped and started again: on a Legion Y700 (TB322FC, Android 16) a restarted stream left
    // Unicorn Overlord's CRI output short in every other callback for good (998 underruns of 2000, 25% of the sound zero padding; the same 200 callbacks a
    // second), where a new stream had none. A stream that failed, stopped itself (HOLD_MAX_S), or did not call back since the stop (the watchdog's retry after
    // a stall) is opened again
    if (u->st && u->requested_device == atomic_load(&g_output_device) && !atomic_load(&u->failed) && AAudioStream_getState(u->st) == AAUDIO_STREAM_STATE_STARTED && (!atomic_load(&u->held) || atomic_load(&u->sample_time) != u->stop_time)) {
        atomic_store(&u->held, 0); return noErr; }
    stop_stream(u); close_stream(u);
    atomic_store(&u->failed, 0); atomic_store(&u->held, 0);
    AAudioStreamBuilder *b; aaudio_result_t r = AAudio_createStreamBuilder(&b);
    if (r != AAUDIO_OK) return r;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    u->requested_device = atomic_load(&g_output_device);
    AAudioStreamBuilder_setDeviceId(b, u->requested_device);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)u->fmt.mSampleRate);
    AAudioStreamBuilder_setChannelCount(b, (int32_t)u->fmt.mChannelsPerFrame);
    AAudioStreamBuilder_setFormat(b, u->fmt.mFormatFlags & kAudioFormatFlagIsFloat ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    // 5 ms callbacks unless the host asks otherwise, as the iOS front end sets its I/O buffer (docs/agents/03-bringup.md "Crackling on iOS only"): left to itself AAudio
    // on the Legion Y700 called back with 6-960 frames, more than Tales of Eternia Remastered's CRI output keeps queued (about 2 blocks), so 2600 of 2000 callbacks
    // underran and 11% of the sound was zero padding; with 240 frames, 0 underruns
    AAudioStreamBuilder_setFramesPerDataCallback(b, u->frames_hint > 0 ? u->frames_hint : (int32_t)(u->fmt.mSampleRate / 200));
    AAudioStreamBuilder_setDataCallback(b, on_data, u);
    AAudioStreamBuilder_setErrorCallback(b, on_error, u);
    r = AAudioStreamBuilder_openStream(b, &u->st);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { u->st = NULL; return r; }
    __android_log_print(ANDROID_LOG_INFO, "dmhost", "AAudio requested device %d, opened device %d", u->requested_device, AAudioStream_getDeviceId(u->st));
    // the device's own rate and channel count must be ours: AAudio converts when they differ (it reports the requested format back)
    r = AAudioStream_requestStart(u->st);
    if (r != AAUDIO_OK) { close_stream(u); return r; }
    return noErr;
}
// A stop holds the stream (silence, see AudioOutputUnitStart): once it returns no callback is inside the host's render callback, and none enters it until
// the next start (held is set before in_cb is read, and each callback counts itself in before it reads held)
OSStatus AudioOutputUnitStop(AudioUnit u) {
    if (!u->st) return noErr;
    u->held_frames = 0; atomic_store(&u->held, 1);
    while (atomic_load(&u->in_cb)) sched_yield();
    u->stop_time = atomic_load(&u->sample_time); return noErr;
}
OSStatus AudioUnitUninitialize(AudioUnit u) { stop_stream(u); close_stream(u); return noErr; }
OSStatus AudioComponentInstanceDispose(AudioComponentInstance u) { if (u) { stop_stream(u); close_stream(u); free(u); } return noErr; }
// AudioConverter (AAC → PCM) is on MediaCodec: dm/android/compat/audioconverter_mediacodec.c
