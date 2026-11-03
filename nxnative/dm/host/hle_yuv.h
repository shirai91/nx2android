// libvpx pictures converted for the game (hle_movie.c): I420 to NV12 (planes dst and uv, stride w) or RGBA, a smaller picture padded with black, a larger one cropped
#pragma once
#include <stdint.h>
#include "vpx/vpx_image.h"
void dm_yuv_nv12(const vpx_image_t *im, uint8_t *dst, uint8_t *uv, int w, int h);
void dm_yuv_rgba(const vpx_image_t *im, uint8_t *dst, int w, int h);
