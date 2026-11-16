// Linux port: H.264 decoding for the movie decoder (hle_movie.c). Not available yet: mavc_new fails and the game plays without movies.
// (To do: FFmpeg or VA-API.) The interface is dm/host/hle_movie_avc.h.
#include "hle_movie_avc.h"

mavc *mavc_new(void) { return NULL; }
void mavc_free(mavc *a) { (void)a; }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) { (void)a; (void)p; (void)n; }
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) { (void)a; (void)p; (void)n; (void)pts_us; return -1; }
int mavc_take(mavc *a, int flush, uint8_t *nv12, int w, int h, int64_t *pts_us) { (void)a; (void)flush; (void)nv12; (void)w; (void)h; (void)pts_us; return 0; }
int mavc_queued(const mavc *a) { (void)a; return 0; }
int mavc_held(const mavc *a) { (void)a; return 0; }
void mavc_set_rgba(mavc *a) { (void)a; }
void mavc_drop(mavc *a) { (void)a; }
