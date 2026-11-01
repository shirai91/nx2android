// Android port: Apple's libcompression is not there. The capture format (gfx/nvn/nvn_capture.c) stores chunks raw when encoding gives nothing, and an LZFSE chunk read here fails as damaged
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef enum { COMPRESSION_LZFSE = 0x801 } compression_algorithm;
static inline size_t compression_encode_buffer(uint8_t *d, size_t dn, const uint8_t *s, size_t sn, void *scratch, compression_algorithm a) { (void)d; (void)dn; (void)s; (void)sn; (void)scratch; (void)a; return 0; }
static inline size_t compression_decode_buffer(uint8_t *d, size_t dn, const uint8_t *s, size_t sn, void *scratch, compression_algorithm a) { (void)d; (void)dn; (void)s; (void)sn; (void)scratch; (void)a; return 0; }
