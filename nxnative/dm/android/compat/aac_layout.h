// AAC channel layout adaptation from nxnative's movie_aac_android.inc.
// AudioToolbox exposes MPEG element order to the guest; MediaCodec returns
// Android speaker order. Synthetic per-speaker tones distinguish the layouts.
#pragma once
#include <stddef.h>
#include <stdint.h>

static inline int aac_asc_bits(const uint8_t *p, size_t n, unsigned bit, unsigned width) {
    if (!p || n < (bit + width + 7) / 8) return -1;
    int value = 0;
    for (unsigned i = 0; i < width; i++, bit++) value = (value << 1) | ((p[bit / 8] >> (7 - bit % 8)) & 1);
    return value;
}
static inline int aac_channel_config(const uint8_t *asc, size_t n) {
    int object = aac_asc_bits(asc, n, 0, 5); if (object < 0) return -1;
    unsigned freq_bit = object == 31 ? 11 : 5;
    int frequency = aac_asc_bits(asc, n, freq_bit, 4); if (frequency < 0) return -1;
    return aac_asc_bits(asc, n, freq_bit + 4 + (frequency == 15 ? 24 : 0), 4);
}
static inline const uint8_t *aac_guest_order(int config, int channels, int mask) {
    static const uint8_t center_first[] = {2,0,1,3,4};
    static const uint8_t surround[] = {2,0,1,4,5,3};
    static const uint8_t wide[] = {2,6,7,0,1,4,5,3};
    if (config >= 3 && config <= 5 && channels == config) return center_first;
    if (config == 6 && channels == 6) return surround;
    // Configuration 7's second front pair follows the rear pair in this
    // observed Android 7.1-wide mask. Never infer it from channel count alone.
    if (config == 7 && channels == 8 && mask == 0x18fc) return wide;
    return NULL;
}
