#include "nvn_bc.h"
#include "nvn_deswizzle.h"
#include <stdlib.h>
#include <string.h>
int test_bc_layouts(void) {
    int cases = 0;
    int targets[] = {1, 2, 4, 8, 9};
    for (unsigned k = 0; k < sizeof(targets)/sizeof(*targets); k++) {
        int target = targets[k];
        nvn_texdesc d = {target, 0x49, 0, 0, 19, 13, target == 8 ? 1 : target == 9 ? 12 : 3, 5, 0, 0};
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        unsigned char *storage = calloc(1, L.size);
        for (unsigned layer = 0; layer < L.layers; layer++) for (unsigned lv = 0; lv < d.levels; lv++) {
            unsigned w = d.width >> lv, h = d.height >> lv, z = target == 2 ? d.depth >> lv : 1;
            if (!w) w = 1; if (!h) h = 1; if (!z) z = 1;
            size_t pitch = ((w + 3)/4) * 8, rows = (h + 3)/4, n = pitch * rows * z;
            unsigned char *blocks = calloc(1, n);
            for (unsigned s = 0; s < z; s++) for (size_t b = 0; b < pitch * rows; b += 8) blocks[s*pitch*rows + b] = 30 + 7*layer + lv + s;
            if (nvn_swizzle(&d, lv, layer, blocks, pitch, storage)) return -1;
            free(blocks);
        }
        for (unsigned layer = 0; layer < L.layers; layer++) for (unsigned lv = 0; lv < d.levels; lv++) {
            unsigned w = d.width >> lv, h = d.height >> lv, z = target == 2 ? d.depth >> lv : 1;
            if (!w) w = 1; if (!h) h = 1; if (!z) z = 1;
            size_t pitch = ((w + 3)/4) * 8, rows = (h + 3)/4, n = pitch * rows * z;
            unsigned char *blocks = malloc(n), *out = malloc(w*h*z);
            if (nvn_deswizzle(&d, lv, layer, storage, blocks, pitch) ||
                nvn_bc_decode(0x49, w, h, z, blocks, n, pitch, pitch*rows, out, w*h*z, w, w*h)) return -2;
            for (unsigned s = 0; s < z; s++) for (unsigned i = 0; i < w*h; i++)
                if (out[s*w*h + i] != 30 + 7*layer + lv + s) return -3;
            cases++; free(blocks); free(out);
        }
        free(storage);
    }
    return cases;
}
