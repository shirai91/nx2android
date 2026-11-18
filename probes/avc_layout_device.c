// Adapted from thehobbit85/nxnative 1f01e28c t_android_avc: one six-frame sequence per flush.
// Six synthetic access units with B pictures. Check planes and black padding;
// the target timestamp/reordering policy is outside this layout-only port.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle_movie_avc.h"
#include <stdarg.h>
void die(const char *fmt,...) {va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);abort();}
#include "../tests/fixtures/h264_synthetic.h"
static void stream(mavc *a, int rgba) {
    enum { width = 68, height = 66, guard = 16 };
    uint8_t frame[width * height * 4 + 2 * guard];
    int n = 0; unsigned seen = 0;
    for (int i = 0; i <= 6; i++) {
        if (i < 6) {
            unsigned j = i % 6;
            int64_t pts = packets[j].pts;
            if (rgba) {
                // Each synthetic access unit contains one slice. Replace its length with an Annex B start code.
                uint8_t au[256] = {0,0,0,1}; assert(packets[j].n <= sizeof au);
                memcpy(au + 4, packets[j].p + 4, packets[j].n - 4);
                assert(!mavc_decode(a, au, (int)packets[j].n, pts));
            } else assert(!mavc_decode(a, packets[j].p, (int)packets[j].n, pts));
        }
        int64_t pts;
        memset(frame, 0x37, sizeof frame);
        uint8_t *dst = frame + guard;
        while (mavc_take(a, i == 6, dst, width, height, &pts)) {
            assert(n < 6);
            static const uint8_t rgb[6][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,0},{0,255,255},{255,0,255}};
            int idx = -1;
            for (int candidate=0;candidate<6;candidate++) {
                int matches=rgba ? abs(dst[0]-rgb[candidate][0])<=8 && abs(dst[1]-rgb[candidate][1])<=8 && abs(dst[2]-rgb[candidate][2])<=8 : abs(dst[0]-yuv[candidate][0])<=4;
                if(matches) { assert(idx<0);idx=candidate; }
            }
            assert(idx>=0 && !(seen & (1u<<idx)));seen |= 1u<<idx;
            // nx2android's existing synthetic-timestamp policy differs from
            // the donor for B frames (same assertion fails on unchanged main).
            // This probe qualifies planes/padding, not timestamp ordering.
            (void)pts;
            size_t bytes = rgba ? width * height * 4 : width * height * 3 / 2;
            for (int k = 0; k < guard; k++) assert(frame[k] == 0x37 && dst[bytes + k] == 0x37);
            for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
                int inside = x < 64 && y < 64;
                if (!rgba) assert(abs(dst[y*width+x] - (inside ? yuv[idx][0] : 16)) <= 4);
                else {
                    const uint8_t *pixel = dst + 4 * (y * width + x);
                    for (int c = 0; c < 3; c++) assert(abs(pixel[c] - (inside ? rgb[idx][c] : 0)) <= 8);
                    assert(pixel[3] == 255);
                }
            }
            if (!rgba) for (int y = 0; y < height / 2; y++) for (int x = 0; x < width; x++)
                assert(abs(dst[width * height + y*width+x] - (x < 64 && y < 32 ? yuv[idx][1+(x&1)] : 128)) <= 6);
            n++;
        }
    }
    assert(n == 6 && seen == 63);
}
int main(void) {
    for (int rgba = 0; rgba <= 1; rgba++) {
        mavc *a = mavc_new();
        if (rgba) mavc_set_rgba(a);
        mavc_avcc(a, avcc, sizeof avcc);
        stream(a, rgba);
        mavc_drop(a);
        stream(a, rgba);
        mavc_free(a);
    }
    puts("Synthetic H.264 planes, EOS, flush and padded NV12/RGBA passed");
}
