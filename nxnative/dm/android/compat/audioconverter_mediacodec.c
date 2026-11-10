// Android port: AudioConverter (AAC → interleaved 16-bit PCM) on MediaCodec, for the SDK's movie library (dm/host/hle_movie.c md_aac), the counterpart of AudioToolbox's AAC decoder.
// Mario vs. Donkey Kong's movies (Movie/*.mp4, H.264 + AAC-LC 2 channels 48 kHz) played without sound while AudioConverterNew failed here.
// hle_movie.c sets the AudioSpecificConfig as an MPEG-4 ES descriptor (the magic cookie, as FFmpeg's AudioToolbox decoder builds it): the decoder configuration is its DecoderSpecificInfo (tag 5),
// handed to MediaCodec as csd-0. MediaCodec decodes asynchronously while AudioToolbox returns the PCM of the packets it was given: each fill queues the packets the input procedure gives,
// waits briefly for the codec's output and keeps what does not fit (or comes late) for the next fill, so the stream stays continuous and in order
#include "dm.h"
#include <AudioToolbox/AudioToolbox.h>
#include <stdlib.h>
#include <string.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include "aac_layout.h"

struct dm_audio_converter {
    AudioStreamBasicDescription in, out;
    uint8_t asc[64]; int nasc;
    AMediaCodec *mc; int failed, red, och;   // och: channels of the codec's output
    int channel_config, channel_mask;      // ASC identity and the current output's speaker mask
    int mpeg_order;                        // experimental, opted into when this converter is created
    int64_t pts; long nin, nout;             // input timestamps (µs, made up from the packet count) and buffers in / out since the start
    uint8_t *pend; size_t npend, cappend;    // decoded PCM not handed out yet
};

OSStatus AudioConverterNew(const AudioStreamBasicDescription *in, const AudioStreamBasicDescription *out, AudioConverterRef *c) {
    *c = NULL;
    if (in->mFormatID != kAudioFormatMPEG4AAC || out->mFormatID != kAudioFormatLinearPCM || out->mBitsPerChannel != 16 || (out->mFormatFlags & (kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved)) ||
        in->mChannelsPerFrame != out->mChannelsPerFrame || !in->mChannelsPerFrame || in->mChannelsPerFrame > 8) return kAudio_UnimplementedError;
    AudioConverterRef a = calloc(1, sizeof *a); if (!a) return -1;
    const char *e = getenv("DM_MOVIE_AAC_MPEG_ORDER"); a->mpeg_order = e && !strcmp(e, "1");
    a->in = *in; a->out = *out; a->och = (int)in->mChannelsPerFrame; *c = a; return noErr;
}
// one descriptor of an ES descriptor: its tag and its body; the length is in 7-bit groups, a set top bit meaning more follow
static const uint8_t *descr(const uint8_t *p, const uint8_t *e, int *tag, size_t *n) {
    if (p >= e) return NULL;
    *tag = *p++; size_t len = 0;
    for (int i = 0; i < 4 && p < e; i++) { uint8_t b = *p++; len = len << 7 | (b & 0x7f); if (!(b & 0x80)) break; }
    if (len > (size_t)(e - p)) return NULL;
    *n = len; return p;
}
OSStatus AudioConverterSetProperty(AudioConverterRef c, AudioConverterPropertyID id, UInt32 size, const void *data) {
    if (id != kAudioConverterDecompressionMagicCookie) return kAudio_UnimplementedError;
    const uint8_t *p = data, *e = p + size; int tag; size_t n;
    // ES descriptor (3): ES ID (2 bytes) and flags (1) before its decoder configuration (4): 13 bytes before its DecoderSpecificInfo (5) = the AudioSpecificConfig
    if (!(p = descr(p, e, &tag, &n)) || tag != 3 || n < 3) return -50;
    e = p + n; p += 3;
    if (!(p = descr(p, e, &tag, &n)) || tag != 4 || n < 13) return -50;
    e = p + n; p += 13;
    if (!(p = descr(p, e, &tag, &n)) || tag != 5 || !n || n > sizeof c->asc) return -50;
    memcpy(c->asc, p, n); c->nasc = (int)n; return noErr;
}
static int open_codec(AudioConverterRef c) {
    if (c->mc) return 1;
    if (c->failed) return 0;
    c->failed = 1;
    int rate = (int)c->in.mSampleRate, ch = (int)c->in.mChannelsPerFrame;
    if (!c->nasc) {   // no cookie: an AAC-LC configuration from the sample rate and channel count
        static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
        int fi = 3; for (int i = 0; i < 13; i++) if (rates[i] == rate) fi = i;
        c->asc[0] = (uint8_t)(2 << 3 | fi >> 1); c->asc[1] = (uint8_t)((fi & 1) << 7 | (ch & 15) << 3); c->nasc = 2;
    }
    c->channel_config = aac_channel_config(c->asc, (size_t)c->nasc); c->channel_mask = 0;
    c->mc = AMediaCodec_createDecoderByType("audio/mp4a-latm");
    if (!c->mc) { LOG("[RED] movie: MediaCodec has no AAC decoder, outputting silence"); return 0; }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, rate); AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, ch);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_IS_ADTS, 0);
    AMediaFormat_setInt32(f, "pcm-encoding", 2);   // AudioFormat.ENCODING_PCM_16BIT (the default; asked for so that no decoder hands out floats)
    AMediaFormat_setBuffer(f, AMEDIAFORMAT_KEY_CSD_0, c->asc, (size_t)c->nasc);
    media_status_t st = AMediaCodec_configure(c->mc, f, NULL, NULL, 0);
    if (st == AMEDIA_OK) st = AMediaCodec_start(c->mc);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK) { LOG("[RED] movie: MediaCodec AAC decoder refused its configuration (%d), outputting silence", (int)st); AMediaCodec_delete(c->mc); c->mc = NULL; return 0; }
    char *name = NULL; AMediaCodec_getName(c->mc, &name);
    LOG("[movie] AAC: MediaCodec decoder %s, %d channels %d Hz, AudioSpecificConfig %d bytes", name ? name : "?", ch, rate, c->nasc);
    if (name) AMediaCodec_releaseName(c->mc, name);
    c->failed = 0; return 1;
}
static void keep(AudioConverterRef c, const uint8_t *b, size_t n) {
    int ch = (int)c->out.mChannelsPerFrame;
    size_t frames = n / (2u * (size_t)c->och), need = c->npend + frames * 2u * (size_t)ch;
    if (need > c->cappend) { c->cappend = need * 2; c->pend = realloc(c->pend, c->cappend); if (!c->pend) die("movie: AAC buffer allocation failed"); }
    int16_t *o = (int16_t *)(c->pend + c->npend); const int16_t *s = (const int16_t *)b;
    const uint8_t *order = c->mpeg_order && c->och == ch ? aac_mpeg_order(c->channel_config, c->och, c->channel_mask) : NULL;
    if (order) for (size_t i = 0; i < frames; i++) for (int k = 0; k < ch; k++)
        memcpy(o + i * ch + k, b + 2 * (i * ch + order[k]), 2);
    else if (c->och == ch) memcpy(o, s, frames * 2u * (size_t)ch);
    else for (size_t i = 0; i < frames; i++) for (int k = 0; k < ch; k++) o[i * ch + k] = s[i * c->och + (k < c->och ? k : c->och - 1)];   // a codec that mixes up or down: the channels it has
    c->npend = need;
}
// collect the codec's output, waiting up to wait_us for the first buffer
static void drain(AudioConverterRef c, int64_t wait_us) {
    for (;;) {
        AMediaCodecBufferInfo bi; ssize_t i = AMediaCodec_dequeueOutputBuffer(c->mc, &bi, wait_us);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *f = AMediaCodec_getOutputFormat(c->mc); int32_t v, r = 0, enc = 2;
            if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v) && v > 0 && v <= 8) c->och = v;
            c->channel_mask = 0; AMediaFormat_getInt32(f, "channel-mask", &c->channel_mask);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &r); AMediaFormat_getInt32(f, "pcm-encoding", &enc);
            if (c->och != (int)c->out.mChannelsPerFrame || (r && r != (int)c->out.mSampleRate) || enc != 2)
                LOG("[RED] movie: the AAC decoder puts out %d channels %d Hz encoding %d for %d channels %d Hz 16-bit", c->och, r, enc, (int)c->out.mChannelsPerFrame, (int)c->out.mSampleRate);
            AMediaFormat_delete(f); continue;
        }
        if (i == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (i < 0) return;   // AMEDIACODEC_INFO_TRY_AGAIN_LATER
        size_t n = 0; uint8_t *b = AMediaCodec_getOutputBuffer(c->mc, (size_t)i, &n);
        if (b && bi.size > 0 && (size_t)bi.offset + (size_t)bi.size <= n) keep(c, b + bi.offset, (size_t)bi.size);
        AMediaCodec_releaseOutputBuffer(c->mc, (size_t)i, false);
        c->nout++; wait_us = 0;
    }
}
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef c, AudioConverterComplexInputDataProc proc, void *ref, UInt32 *packets, AudioBufferList *out, AudioStreamPacketDescription *desc) {
    (void)desc;
    UInt32 want = *packets; *packets = 0;
    if (!open_codec(c)) return kAudio_UnimplementedError;
    size_t fb = 2u * c->out.mChannelsPerFrame, cap = out->mBuffers[0].mDataByteSize / fb;
    if (want > cap) want = (UInt32)cap;
    OSStatus st = noErr;
    // packets in while what is decoded or in flight is short of the request (1024 frames per AAC-LC packet), and always the first one offered: hle_movie.c offers one packet per fill
    // and asks for one packet's frames, so a packet left untaken would be lost
    for (int first = 1; first || c->npend / fb + (size_t)(c->nin - c->nout) * 1024 < want; first = 0) {
        UInt32 np = 1; AudioBufferList bl = {1, {{c->in.mChannelsPerFrame, 0, NULL}}}; AudioStreamPacketDescription *pd = NULL;
        st = proc(c, &np, &bl, &pd, ref);
        if (st || !np || !bl.mBuffers[0].mData || !bl.mBuffers[0].mDataByteSize) break;
        ssize_t i = -1;
        for (int k = 0; k < 50 && i < 0; k++) { i = AMediaCodec_dequeueInputBuffer(c->mc, 2000); if (i < 0) drain(c, 0); }
        if (i < 0) { if (!c->red++) LOG("[RED] movie: the AAC decoder took no input for 100 ms, a packet dropped"); continue; }
        size_t n = 0; uint8_t *b = AMediaCodec_getInputBuffer(c->mc, (size_t)i, &n);
        UInt32 len = bl.mBuffers[0].mDataByteSize; if (len > n) len = (UInt32)n;
        if (b) memcpy(b, bl.mBuffers[0].mData, len);
        AMediaCodec_queueInputBuffer(c->mc, (size_t)i, 0, b ? len : 0, (uint64_t)c->pts, 0);
        c->pts += 1024 * 1000000ll / (int64_t)(c->in.mSampleRate > 0 ? c->in.mSampleRate : 48000); c->nin++;
    }
    // the decoder's output. c2.android.aac.decoder (Legion Y700) keeps two packets in flight and answers within about 1 ms (measured over the first 60 fills): only more in flight than that
    // is waited for; what comes later goes out with the next fill
    drain(c, c->nin > c->nout + 2 ? 20000 : 0);
    size_t frames = c->npend / fb; if (frames > want) frames = want;
    memcpy(out->mBuffers[0].mData, c->pend, frames * fb);
    memmove(c->pend, c->pend + frames * fb, c->npend - frames * fb); c->npend -= frames * fb;
    out->mBuffers[0].mDataByteSize = (UInt32)(frames * fb);
    *packets = (UInt32)frames;
    return frames ? noErr : st;
}
// The PCM still in the codec once no packet comes (hle_movie.c md_aac_tail): an end-of-stream input drains it, the codec is closed (the next fill opens it afresh, should
// the stream go on), and up to cap bytes of what is decoded go out per call
size_t dm_aac_tail(AudioConverterRef c, void *dst, size_t cap) {
    if (c->mc && c->nin > c->nout) {
        ssize_t i = AMediaCodec_dequeueInputBuffer(c->mc, 20000);
        if (i >= 0) {
            AMediaCodec_queueInputBuffer(c->mc, (size_t)i, 0, 0, (uint64_t)c->pts, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            for (int k = 0; k < 10 && c->nin > c->nout; k++) drain(c, 10000);   // the end-of-stream output carries no PCM and is counted too
        }
        AMediaCodec_stop(c->mc); AMediaCodec_delete(c->mc); c->mc = NULL; c->nin = c->nout = 0;
    }
    size_t fb = 2u * c->out.mChannelsPerFrame, n = c->npend < cap ? c->npend : cap; n -= n % fb;
    memcpy(dst, c->pend, n); memmove(c->pend, c->pend + n, c->npend - n); c->npend -= n;
    return n;
}
OSStatus AudioConverterDispose(AudioConverterRef c) {
    if (!c) return noErr;
    if (c->mc) { AMediaCodec_stop(c->mc); AMediaCodec_delete(c->mc); }
    free(c->pend); free(c); return noErr;
}
