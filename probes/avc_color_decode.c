// Synthetic AVC probe: production adapter and real MediaCodec, with an optional
// named codec to separate conversion tests from emulator/vendor decoder quirks.
#include <media/NdkMediaCodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static const char *codec_name;
static AMediaCodec *probe_decoder(const char *mime) {
    return codec_name ? AMediaCodec_createCodecByName(codec_name) : AMediaCodec_createDecoderByType(mime);
}
#define AMediaCodec_createDecoderByType probe_decoder
#include "../nxnative/dm/android/movie_avc.c"
#undef AMediaCodec_createDecoderByType

static uint32_t read_u32(FILE *f) {
    uint8_t b[4]; assert(fread(b,1,4,f) == 4);
    return (uint32_t)b[0]<<24 | (uint32_t)b[1]<<16 | (uint32_t)b[2]<<8 | b[3];
}
static void write_picture(mavc *a, FILE *out, uint8_t *pixels, int w, int h, int flush, int *frames) {
    int64_t pts;
    while (mavc_take(a,flush,pixels,w,h,&pts)) {
        fprintf(stderr,"frame %d: format standard=%d full=%d first RGB=%u,%u,%u\n",*frames,a->fstandard,a->ffull,pixels[0],pixels[1],pixels[2]);
        assert(pts == (int64_t)*frames*40000);
        assert(fwrite(pixels,4,(size_t)w*h,out) == (size_t)w*h);
        (*frames)++;
    }
}
int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr,"usage: avc_color_decode packets output.rgba codec-name-or-default\n"); return 2; }
    codec_name = strcmp(argv[3],"default") ? argv[3] : NULL;
    FILE *in = fopen(argv[1],"rb"), *out = fopen(argv[2],"wb"); assert(in && out);
    assert(read_u32(in) == 0x41564350); // AVCP, big-endian dimensions/count/packet lengths
    int w = (int)read_u32(in), h = (int)read_u32(in), count = (int)read_u32(in);
    assert(w > 0 && w <= 4096 && h > 0 && h <= 4096 && count > 0 && count <= 16);
    uint8_t *pixels = malloc((size_t)w*h*4); assert(pixels);
    mavc *a = mavc_new(); mavc_set_rgba(a); int frames = 0;
    for (int i=0; i<count; i++) {
        uint32_t size = read_u32(in); assert(size && size < (4u<<20));
        uint8_t *packet = malloc(size); assert(packet && fread(packet,1,size,in) == size);
        assert(!mavc_decode(a,packet,(int)size,(int64_t)i*40000)); free(packet);
        write_picture(a,out,pixels,w,h,0,&frames);
    }
    write_picture(a,out,pixels,w,h,1,&frames);
    assert(frames == count && fgetc(in) == EOF);
    mavc_free(a); free(pixels); assert(!fclose(in) && !fclose(out));
    fprintf(stderr,"AVC: %d RGBA frames from %s\n",frames,argv[3]);
    return 0;
}
