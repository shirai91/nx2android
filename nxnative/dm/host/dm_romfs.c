// See dm_romfs.h. Everything here is plain POSIX (open/pread) and freestanding of the emulator, so a test can link it as is.
// One container is one RomFS; a BKTR update is mounted as a view over the base NCA: virtual offsets of the update section are
// resolved through the update's relocation table to either the update's own patch data (subsection CTR values) or the base NCA's
// RomFS section. Both containers are the player's official files; nothing is unpacked.
#include "dm_romfs.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int fd, valid;
    uint64_t nca_base, section_off, section_size;
    uint64_t romfs_off;       // RomFS image inside the section (IVFC level 5)
    uint8_t ctr[8], key[16];
    int crypt;                // 1 none, 3 CTR, 4 BKTR (patch section; its tables still read as plain CTR)
} sect_t;
typedef struct { uint64_t virt, phys; int patch; } reloc_t;
typedef struct { uint64_t off; uint32_t ctr; } sub_t;
typedef struct { uint32_t off, parent; } idx_ent;

struct romfs_cont {
    sect_t base;              // the single section, or the base NCA's section in a BKTR view
    sect_t patch;             // the BKTR update's section (bktr only)
    int bktr;
    reloc_t *rel; size_t nrel;   // sorted by virt, sentinel (total virtual size) last
    sub_t *sub; size_t nsub;     // sorted by physical offset, sentinels last
    uint64_t romfs_off;       // view-level RomFS image offset
    uint64_t data_off;        // file data inside the RomFS image
    uint8_t *dirs, *files;    // RomFS metadata tables (decrypted, resident)
    uint64_t dirs_n, files_n;
    idx_ent *dix, *fix;       // entries sorted by (parent, offset)
    size_t ndix, nfix;
    char err[256];
    pthread_mutex_t lock;
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr64be(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[7 - i] = (uint8_t)(v >> (8 * i)); }
static void wr32be(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[3 - i] = (uint8_t)(v >> (8 * i)); }

static void seterr(romfs_cont *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void seterr(romfs_cont *c, const char *fmt, ...) {
    if (!c) return;
    pthread_mutex_lock(&c->lock);
    va_list ap; va_start(ap, fmt); vsnprintf(c->err, sizeof c->err, fmt, ap); va_end(ap);
    pthread_mutex_unlock(&c->lock);
}
const char *romfs_error(const romfs_cont *c) { return c ? c->err : ""; }

// ---------- AES-128: T-table encryption (CTR needs the speed), byte-wise inverse cipher (header/key area only) ----------
static const uint8_t S[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };
static uint8_t Si[256];
static uint32_t Te0[256], Te1[256], Te2[256], Te3[256];
static pthread_once_t aes_once = PTHREAD_ONCE_INIT;

static uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) { uint8_t r = 0; while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; } return r; }
static void aes_tables(void) {
    for (int i = 0; i < 256; i++)
        for (int j = 0; j < 256; j++)
            if (S[j] == i) { Si[i] = (uint8_t)j; break; }
    for (int i = 0; i < 256; i++) {
        uint8_t s = S[i], s2 = gmul(s, 2), s3 = (uint8_t)(s2 ^ s);
        Te0[i] = (uint32_t)s2 << 24 | (uint32_t)s << 16 | (uint32_t)s << 8 | s3;
        Te1[i] = ror32(Te0[i], 8); Te2[i] = ror32(Te0[i], 16); Te3[i] = ror32(Te0[i], 24);
    }
}
typedef struct { uint32_t rk[44]; } aes128;
static void aes_expand(aes128 *k, const uint8_t key[16]) {
    pthread_once(&aes_once, aes_tables);
    static const uint8_t rcon[11] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 27, 54};
    for (int i = 0; i < 4; i++) k->rk[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 | (uint32_t)key[4 * i + 2] << 8 | key[4 * i + 3];
    for (int i = 4; i < 44; i++) {
        uint32_t t = k->rk[i - 1];
        if (!(i & 3)) {
            t = t << 8 | t >> 24;   // RotWord: the bytes rotate left by one
            t = (uint32_t)S[t >> 24] << 24 | (uint32_t)S[(t >> 16) & 0xff] << 16 | (uint32_t)S[(t >> 8) & 0xff] << 8 | S[t & 0xff];
            t ^= (uint32_t)rcon[i >> 2] << 24;
        }
        k->rk[i] = k->rk[i - 4] ^ t;
    }
}
static void aes_enc(const aes128 *k, const uint8_t in[16], uint8_t out[16]) {
    uint32_t t0 = ((uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3]) ^ k->rk[0];
    uint32_t t1 = ((uint32_t)in[4] << 24 | (uint32_t)in[5] << 16 | (uint32_t)in[6] << 8 | in[7]) ^ k->rk[1];
    uint32_t t2 = ((uint32_t)in[8] << 24 | (uint32_t)in[9] << 16 | (uint32_t)in[10] << 8 | in[11]) ^ k->rk[2];
    uint32_t t3 = ((uint32_t)in[12] << 24 | (uint32_t)in[13] << 16 | (uint32_t)in[14] << 8 | in[15]) ^ k->rk[3];
    for (int r = 1; r <= 9; r++) {
        uint32_t s0 = Te0[t0 >> 24] ^ Te1[(t1 >> 16) & 0xff] ^ Te2[(t2 >> 8) & 0xff] ^ Te3[t3 & 0xff] ^ k->rk[4 * r];
        uint32_t s1 = Te0[t1 >> 24] ^ Te1[(t2 >> 16) & 0xff] ^ Te2[(t3 >> 8) & 0xff] ^ Te3[t0 & 0xff] ^ k->rk[4 * r + 1];
        uint32_t s2 = Te0[t2 >> 24] ^ Te1[(t3 >> 16) & 0xff] ^ Te2[(t0 >> 8) & 0xff] ^ Te3[t1 & 0xff] ^ k->rk[4 * r + 2];
        uint32_t s3 = Te0[t3 >> 24] ^ Te1[(t0 >> 16) & 0xff] ^ Te2[(t1 >> 8) & 0xff] ^ Te3[t2 & 0xff] ^ k->rk[4 * r + 3];
        t0 = s0; t1 = s1; t2 = s2; t3 = s3;
    }
    uint32_t w0 = ((uint32_t)S[t0 >> 24] << 24 | (uint32_t)S[(t1 >> 16) & 0xff] << 16 | (uint32_t)S[(t2 >> 8) & 0xff] << 8 | S[t3 & 0xff]) ^ k->rk[40];
    uint32_t w1 = ((uint32_t)S[t1 >> 24] << 24 | (uint32_t)S[(t2 >> 16) & 0xff] << 16 | (uint32_t)S[(t3 >> 8) & 0xff] << 8 | S[t0 & 0xff]) ^ k->rk[41];
    uint32_t w2 = ((uint32_t)S[t2 >> 24] << 24 | (uint32_t)S[(t3 >> 16) & 0xff] << 16 | (uint32_t)S[(t0 >> 8) & 0xff] << 8 | S[t1 & 0xff]) ^ k->rk[42];
    uint32_t w3 = ((uint32_t)S[t3 >> 24] << 24 | (uint32_t)S[(t0 >> 16) & 0xff] << 16 | (uint32_t)S[(t1 >> 8) & 0xff] << 8 | S[t2 & 0xff]) ^ k->rk[43];
    for (int i = 0; i < 4; i++) { out[i] = (uint8_t)(w0 >> (24 - 8 * i)); out[4 + i] = (uint8_t)(w1 >> (24 - 8 * i)); out[8 + i] = (uint8_t)(w2 >> (24 - 8 * i)); out[12 + i] = (uint8_t)(w3 >> (24 - 8 * i)); }
}
static void aes_dec(const aes128 *k, const uint8_t in[16], uint8_t out[16]) {
    uint8_t st[16], t[16];
    for (int i = 0; i < 16; i++) st[i] = in[i] ^ (uint8_t)(k->rk[40 + i / 4] >> (24 - 8 * (i % 4)));
    for (int r = 9; r >= 1; r--) {
        for (int i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = st[i];   // InvShiftRows: row r shifts right by r (state[r][c] = i, c = i/4)
        for (int i = 0; i < 16; i++) st[i] = Si[t[i]];
        for (int i = 0; i < 16; i++) st[i] ^= (uint8_t)(k->rk[4 * r + i / 4] >> (24 - 8 * (i % 4)));
        for (int c = 0; c < 4; c++) {
            uint8_t a0 = st[4 * c], a1 = st[4 * c + 1], a2 = st[4 * c + 2], a3 = st[4 * c + 3];
            st[4 * c] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
            st[4 * c + 1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
            st[4 * c + 2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
            st[4 * c + 3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
        }
    }
    for (int i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = st[i];
    for (int i = 0; i < 16; i++) out[i] = Si[t[i]] ^ (uint8_t)(k->rk[i / 4] >> (24 - 8 * (i % 4)));
}
static void aes_ecb_dec(const aes128 *k, uint8_t *buf, size_t n) {
    for (size_t o = 0; o < n; o += 16) { uint8_t b[16]; memcpy(b, buf + o, 16); aes_dec(k, b, buf + o); }
}
// Nintendo's XTS: a 128-bit big-endian tweak per 0x200-byte sector, K1 = the first 16 bytes, K2 the second
static void xts_mul(uint8_t t[16]) {
    unsigned carry = 0;
    for (int i = 0; i < 16; i++) { unsigned v = t[i]; t[i] = (uint8_t)((v << 1) | carry); carry = v >> 7; }
    if (carry) t[0] ^= 0x87;
}
static void xts_decrypt(const uint8_t key[32], uint64_t sector, uint8_t *data, size_t n) {
    aes128 k1, k2; aes_expand(&k1, key); aes_expand(&k2, key + 16);
    uint8_t t[16] = {0}, et[16];
    wr64be(t + 8, sector);
    aes_enc(&k2, t, et);
    memcpy(t, et, 16);
    for (size_t o = 0; o < n; o += 16) {
        uint8_t b[16], d[16];
        memcpy(b, data + o, 16);
        for (int i = 0; i < 16; i++) b[i] ^= t[i];
        aes_dec(&k1, b, d);
        for (int i = 0; i < 16; i++) data[o + i] = d[i] ^ t[i];
        xts_mul(t);
    }
}

// ---------- prod.keys ----------
static int hexval(int ch) { if (ch >= '0' && ch <= '9') return ch - '0'; if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10; if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10; return -1; }
// "name = <hex>" in a prod.keys file; returns the byte length (0 when absent)
static size_t key_find(const char *keys, const char *name, uint8_t *out, size_t cap) {
    FILE *f = fopen(keys, "r");
    if (!f) return 0;
    size_t nl = strlen(name); char line[512]; size_t got = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || !*p || strncmp(p, name, nl)) continue;
        if (p[nl] != ' ' && p[nl] != '\t' && p[nl] != '=') continue;
        p += nl; while (*p == ' ' || *p == '\t') p++; if (*p != '=') continue; p++;
        while (*p == ' ' || *p == '\t') p++;
        size_t n = 0;
        while (*p && *p != '\r' && *p != '\n' && *p != '#' && *p != ' ' && *p != '\t') {
            int hi = hexval(p[0]), lo = p[1] ? hexval(p[1]) : -1;
            if (hi < 0 || lo < 0 || n >= cap) break;
            out[n++] = (uint8_t)(hi << 4 | lo); p += 2;
        }
        got = n; break;
    }
    fclose(f);
    return got;
}

// ---------- section reads ----------
static int pread_all(int fd, void *buf, size_t n, uint64_t off) {
    size_t got = 0;
    while (got < n) { ssize_t r = pread(fd, (uint8_t *)buf + got, n - got, (off_t)(off + got)); if (r < 0 && errno == EINTR) continue; if (r <= 0) return -1; got += (size_t)r; }
    return 0;
}
#define CTR_CHUNK (1u << 20)
// IV for a section CTR read: the section counter reversed in bytes 0..7, the block index big-endian in bytes 8..15
static void iv_ctr(uint8_t iv[16], const uint8_t sc[8], uint64_t block) {
    for (int j = 0; j < 8; j++) iv[j] = sc[7 - j];
    wr64be(iv + 8, block);
}
// BKTR patch IV: bytes 0..3 are the section counter's high half, 4..7 the subsection's ctr value, 8..15 the block index
static void iv_bktr(uint8_t iv[16], const uint8_t sc[8], uint32_t ctr_val, uint64_t block) {
    for (int j = 0; j < 4; j++) iv[j] = sc[7 - j];
    wr32be(iv + 4, ctr_val);
    wr64be(iv + 8, block);
}
// Section-relative read; CTR decrypts in 1 MiB chunks, in place when the position and the request are 16-byte aligned.
// ctr_val < 0: an ordinary section read; otherwise a BKTR patch read with that subsection's ctr value.
static int sect_read_ex(romfs_cont *c, const sect_t *s, int64_t ctr_val, uint64_t rel, void *buf, size_t n, size_t *got) {
    *got = 0;
    if (!n) return 0;
    if (s->crypt == 1) {
        if (pread_all(s->fd, buf, n, s->nca_base + s->section_off + rel)) { seterr(c, "container read failed at %#llx+%#zx: %s", (unsigned long long)rel, n, strerror(errno)); return -1; }
        *got = n; return 0;
    }
    aes128 k; aes_expand(&k, s->key);
    uint8_t *scratch = NULL;
    for (size_t done = 0; done < n;) {
        size_t chunk = n - done < CTR_CHUNK ? n - done : CTR_CHUNK;
        uint64_t abs = s->section_off + rel + done, start = abs & ~15ull;
        size_t skip = (size_t)(abs - start);
        if (!skip && !(chunk & 15)) {
            if (pread_all(s->fd, (uint8_t *)buf + done, chunk, s->nca_base + start)) { free(scratch); seterr(c, "container read failed at %#llx+%#zx: %s", (unsigned long long)(rel + done), chunk, strerror(errno)); return -1; }
            uint8_t iv[16], ks[16];
            if (ctr_val < 0) iv_ctr(iv, s->ctr, start >> 4); else iv_bktr(iv, s->ctr, (uint32_t)ctr_val, start >> 4);
            for (size_t o = 0; o < chunk; o += 16) {
                wr64be(iv + 8, (start + o) >> 4);
                aes_enc(&k, iv, ks);
                for (size_t i = 0; i < 16; i++) ((uint8_t *)buf)[done + o + i] ^= ks[i];
            }
        } else {
            size_t len = (skip + chunk + 15) & ~(size_t)15;
            if (!scratch && !(scratch = malloc(CTR_CHUNK + 16))) { seterr(c, "out of memory reading %#zx bytes", n); return -1; }
            if (pread_all(s->fd, scratch, len, s->nca_base + start)) { free(scratch); seterr(c, "container read failed at %#llx+%#zx: %s", (unsigned long long)(rel + done), len, strerror(errno)); return -1; }
            uint8_t iv[16], ks[16];
            if (ctr_val < 0) iv_ctr(iv, s->ctr, start >> 4); else iv_bktr(iv, s->ctr, (uint32_t)ctr_val, start >> 4);
            for (size_t o = 0; o < len; o += 16) {
                wr64be(iv + 8, (start + o) >> 4);
                aes_enc(&k, iv, ks);
                for (size_t i = 0; i < 16; i++) scratch[o + i] ^= ks[i];
            }
            memcpy((uint8_t *)buf + done, scratch + skip, chunk);
        }
        done += chunk;
    }
    free(scratch);
    *got = n; return 0;
}
static int sect_read(romfs_cont *c, const sect_t *s, uint64_t rel, void *buf, size_t n, size_t *got) { return sect_read_ex(c, s, -1, rel, buf, n, got); }

// ---------- NCA discovery and section parsing ----------
static int nca_header_ok(const uint8_t *hdr) { return !memcmp(hdr + 0x200, "NCA3", 4) || !memcmp(hdr + 0x200, "NCA2", 4); }
typedef struct { uint64_t base, size; const char *name; } nca_ent;
// Picks the program NCA that carries a RomFS section and returns its file offset inside the container: content type 0 (the program),
// then the largest NCA (the one holding the RomFS data)
static int pick_nca(int fd, const uint8_t hkey[32], const nca_ent *ents, uint32_t cnt, uint64_t *nca_base, char *name, size_t name_n, char *err, size_t errn) {
    uint64_t best = 0, best_sz = 0;
    int found = 0, best_program = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        if (ents[i].size < 0x1000) continue;
        uint8_t hdr[0xc00];
        if (pread_all(fd, hdr, sizeof hdr, ents[i].base)) continue;
        for (int k = 0; k < 6; k++) xts_decrypt(hkey, (uint64_t)k, hdr + k * 0x200, 0x200);
        if (!nca_header_ok(hdr)) continue;
        int rom = -1;
        for (int k = 0; k < 4; k++) if (rd32(hdr + 0x240 + k * 0x10) && hdr[0x400 + k * 0x200 + 3] == 3) { rom = k; break; }
        if (rom < 0) continue;
        int program = hdr[0x205] == 0;
        if (!found || (program && !best_program) || (program == best_program && ents[i].size > best_sz)) {
            best = ents[i].base; best_sz = ents[i].size; found = 1; best_program = program;
            snprintf(name, name_n, "%s", ents[i].name);
        }
    }
    if (!found) { snprintf(err, errn, "no NCA carries a RomFS section"); return -1; }
    *nca_base = best;
    return 0;
}
// An NSP: PFS0 entries are the NCAs
static int pfs0_find_nca(int fd, const uint8_t hkey[32], uint64_t *nca_base, char *name, size_t name_n, char *err, size_t errn) {
    uint8_t h[0x10];
    if (pread_all(fd, h, sizeof h, 0) || memcmp(h, "PFS0", 4)) { snprintf(err, errn, "not a PFS0 (NSP) container"); return -1; }
    uint32_t cnt = rd32(h + 4), strsz = rd32(h + 8);
    if (!cnt || cnt > 4096 || strsz > (4u << 20)) { snprintf(err, errn, "PFS0 header %u entries, %u name bytes", cnt, strsz); return -1; }
    uint8_t *ents = malloc((size_t)cnt * 0x18); char *names = malloc((size_t)strsz + 1); nca_ent *list = malloc((size_t)cnt * sizeof *list);
    int rc = -1;
    if (!ents || !names || !list || pread_all(fd, ents, (size_t)cnt * 0x18, 0x10) || pread_all(fd, names, strsz, 0x10 + (uint64_t)cnt * 0x18)) { snprintf(err, errn, "NSP header unreadable"); goto out; }
    names[strsz] = 0;
    uint64_t data = 0x10 + (uint64_t)cnt * 0x18 + strsz;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t no = rd32(ents + (size_t)i * 0x18 + 0x10);
        list[i].base = data + rd64(ents + (size_t)i * 0x18); list[i].size = rd64(ents + (size_t)i * 0x18 + 8);
        list[i].name = no < strsz ? names + no : "";
    }
    rc = pick_nca(fd, hkey, list, cnt, nca_base, name, name_n, err, errn);
out:
    free(ents); free(names); free(list);
    return rc;
}
// A gamecard image (XCI): the root HFS0 holds the secure partition (an HFS0 of NCAs); the header's plaintext part gives the root's offset.
// Gamecard NCAs differ from an NSP's only in distribution type: the key area still decrypts with key_area_key_application.
static int xci_find_nca(int fd, const uint8_t hkey[32], uint64_t *nca_base, char *name, size_t name_n, char *err, size_t errn) {
    uint8_t h[0x200], t[0x10];
    struct stat st;
    if (pread_all(fd, h, sizeof h, 0) || memcmp(h + 0x100, "HEAD", 4)) { snprintf(err, errn, "not a gamecard (XCI) image"); return -1; }
    if (fstat(fd, &st)) { snprintf(err, errn, "container size: %s", strerror(errno)); return -1; }
    uint64_t fsz = (uint64_t)st.st_size, root = rd64(h + 0x130);
    if (root > fsz || pread_all(fd, t, sizeof t, root) || memcmp(t, "HFS0", 4)) { snprintf(err, errn, "XCI root partition"); return -1; }
    uint32_t rn = rd32(t + 4), rstr = rd32(t + 8);
    if (!rn || rn > 4 || rstr > (1u << 20)) { snprintf(err, errn, "XCI root HFS0: %u entries", rn); return -1; }
    uint64_t rsz = 0x10 + (uint64_t)rn * 0x40 + rstr, sec = 0, secsz = 0;
    uint8_t *rb = malloc(rsz);
    int rc = -1;
    if (!rb || root + rsz > fsz || pread_all(fd, rb, rsz, root)) { snprintf(err, errn, "XCI root HFS0 unreadable"); goto out; }
    for (uint32_t i = 0; i < rn; i++) {
        const uint8_t *e = rb + 0x10 + (size_t)i * 0x40;
        uint32_t no = rd32(e + 0x10);
        if (no >= rstr || strcmp((const char *)rb + 0x10 + (size_t)rn * 0x40 + no, "secure")) continue;
        uint64_t o = root + rsz + rd64(e), z = rd64(e + 8);
        if (z < 0x1000 || o >= fsz || o + z > fsz) { snprintf(err, errn, "XCI secure partition bounds"); goto out; }
        sec = o; secsz = z; break;
    }
    if (!sec) { snprintf(err, errn, "the XCI has no secure partition"); goto out; }
    if (pread_all(fd, t, sizeof t, sec) || memcmp(t, "HFS0", 4)) { snprintf(err, errn, "XCI secure partition header"); goto out; }
    uint32_t sn = rd32(t + 4), sstr = rd32(t + 8);
    uint64_t shsz = 0x10 + (uint64_t)sn * 0x40 + sstr;
    uint8_t *sb = NULL; nca_ent *list = NULL;
    if (!sn || sn > 4096 || sstr > (1u << 20) || shsz > secsz) { snprintf(err, errn, "XCI secure HFS0: %u entries", sn); goto out; }
    sb = malloc(shsz); list = malloc((size_t)sn * sizeof *list);
    if (!sb || !list || pread_all(fd, sb, shsz, sec)) { snprintf(err, errn, "XCI secure HFS0 unreadable"); goto sec_out; }
    for (uint32_t i = 0; i < sn; i++) {
        const uint8_t *e = sb + 0x10 + (size_t)i * 0x40;
        uint32_t no = rd32(e + 0x10);
        list[i].base = sec + shsz + rd64(e); list[i].size = rd64(e + 8);
        list[i].name = no < sstr ? (const char *)sb + 0x10 + (size_t)sn * 0x40 + no : "";
    }
    rc = pick_nca(fd, hkey, list, sn, nca_base, name, name_n, err, errn);
sec_out:
    free(sb); free(list);
out:
    free(rb);
    return rc;
}
// The container's format: an NSP (PFS0) or a gamecard image (XCI, "HEAD" at 0x100)
static int container_find_nca(int fd, const uint8_t hkey[32], uint64_t *nca_base, char *name, size_t name_n, char *err, size_t errn) {
    uint8_t h[0x104];
    if (pread_all(fd, h, sizeof h, 0)) { snprintf(err, errn, "container unreadable: %s", strerror(errno)); return -1; }
    if (!memcmp(h, "PFS0", 4)) return pfs0_find_nca(fd, hkey, nca_base, name, name_n, err, errn);
    if (!memcmp(h + 0x100, "HEAD", 4)) return xci_find_nca(fd, hkey, nca_base, name, name_n, err, errn);
    snprintf(err, errn, "not an NSP or an XCI (no PFS0 / HEAD magic)");
    return -1;
}
// title.keys sits beside prod.keys: "<rights id, 32 hex> = <title key, 32 hex>"
static int title_key_find(const char *keys_path, const uint8_t rights[16], uint8_t out[16]) {
    char path[1024]; size_t n = strlen(keys_path);
    if (n >= sizeof path) return 0;
    memcpy(path, keys_path, n + 1);
    char *slash = strrchr(path, '/');
    if (slash) snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "title.keys");
    else snprintf(path, sizeof path, "title.keys");
    char lname[33], uname[33], *hx = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { lname[2 * i] = hx[rights[i] >> 4]; lname[2 * i + 1] = hx[rights[i] & 15]; }
    for (int i = 0; i < 32; i++) uname[i] = (char)(lname[i] >= 'a' && lname[i] <= 'f' ? lname[i] - 32 : lname[i]);
    lname[32] = uname[32] = 0;
    return key_find(path, lname, out, 16) == 16 || key_find(path, uname, out, 16) == 16;
}
// The container's own ticket (a .tik entry named after the rights id) holds the title key encrypted with titlekek: no title.keys needed
static int ticket_title_key(int fd, const uint8_t rights[16], const uint8_t kek[32], uint8_t out[16]) {
    uint8_t h[0x10];
    if (pread_all(fd, h, sizeof h, 0) || memcmp(h, "PFS0", 4)) return 0;
    uint32_t cnt = rd32(h + 4), strsz = rd32(h + 8);
    if (!cnt || cnt > 4096) return 0;
    uint8_t *ents = malloc((size_t)cnt * 0x18); char *names = malloc((size_t)strsz + 1);
    int ok = 0;
    if (!ents || !names || pread_all(fd, ents, (size_t)cnt * 0x18, 0x10) || pread_all(fd, names, strsz, 0x10 + (uint64_t)cnt * 0x18)) goto out;
    names[strsz] = 0;
    { char rid[33]; static const char *hx = "0123456789abcdef";
      for (int i = 0; i < 16; i++) { rid[2 * i] = hx[rights[i] >> 4]; rid[2 * i + 1] = hx[rights[i] & 15]; }
      rid[32] = 0;
      uint64_t data = 0x10 + (uint64_t)cnt * 0x18 + strsz;
      for (uint32_t i = 0; i < cnt && !ok; i++) {
          const char *nm = names + rd32(ents + (size_t)i * 0x18 + 0x10);
          size_t nl = strlen(nm);
          if (nl < 4 || strcasecmp(nm + nl - 4, ".tik") || strncasecmp(nm, rid, 32)) continue;
          uint8_t tik[0x190];
          if (pread_all(fd, tik, sizeof tik, data + rd64(ents + (size_t)i * 0x18))) continue;
          aes128 k; aes_expand(&k, kek);
          memcpy(out, tik + 0x180, 16);
          aes_ecb_dec(&k, out, 16);
          ok = 1;
      }
    }
out:
    free(ents); free(names);
    return ok;
}
// Parses the RomFS section of the NCA at the given file offset: key area, CTR/CTR-BKTR, IVFC level 5, section bounds
static int parse_section(romfs_cont *c, sect_t *s, const char *keys, char *err, size_t errn) {
    uint8_t hdr[0xc00];
    if (pread_all(s->fd, hdr, sizeof hdr, s->nca_base)) { snprintf(err, errn, "NCA header unreadable: %s", strerror(errno)); return -1; }
    uint8_t hkey[32];
    if (key_find(keys, "header_key", hkey, sizeof hkey) < 32) { snprintf(err, errn, "header_key is missing from %s", keys); return -1; }
    for (int k = 0; k < 6; k++) xts_decrypt(hkey, (uint64_t)k, hdr + k * 0x200, 0x200);
    if (!nca_header_ok(hdr)) { snprintf(err, errn, "NCA header magic"); return -1; }
    int has_rights = 0;
    for (int i = 0; i < 0x10; i++) has_rights |= hdr[0x230 + i];
    int rom = -1;
    for (int k = 0; k < 4; k++) if (rd32(hdr + 0x240 + k * 0x10) && hdr[0x400 + k * 0x200 + 3] == 3) { rom = k; break; }
    if (rom < 0) { snprintf(err, errn, "no RomFS section in the NCA"); return -1; }
    const uint8_t *fs = hdr + 0x400 + rom * 0x200;
    s->crypt = fs[0x04];
    s->section_off = (uint64_t)rd32(hdr + 0x240 + rom * 0x10) * 0x200;
    s->section_size = (uint64_t)rd32(hdr + 0x240 + rom * 0x10 + 4) * 0x200 - s->section_off;
    s->romfs_off = rd64(fs + 0x08 + 0x10 + 5 * 0x18);   // IVFC level 5 logical offset
    memcpy(s->ctr, fs + 0x140, 8);
    s->valid = 1;
    if (s->crypt != 1 && s->crypt != 3 && s->crypt != 4) { snprintf(err, errn, "unsupported NCA section crypto %d", s->crypt); return -1; }
    if (s->crypt == 1) return 0;
    unsigned keygen = hdr[0x206] > hdr[0x220] ? hdr[0x206] : hdr[0x220];
    unsigned rev = keygen ? keygen - 1 : 0, area = hdr[0x207];
    char kn[64];
    if (has_rights) {   // an eShop update: the content key is the ticket's title key, decrypted with titlekek
        // The ticket's key generation is the rights id's last byte (as in title.keys); the titlekek index is one below it
        unsigned trev = hdr[0x23F] ? hdr[0x23F] - 1 : 0;
        uint8_t enc[16], kek[32];
        snprintf(kn, sizeof kn, "titlekek_%02x", trev);
        if (key_find(keys, kn, kek, sizeof kek) < 16) { snprintf(err, errn, "%s is missing from %s", kn, keys); return -1; }
        if (title_key_find(keys, hdr + 0x230, enc)) {   // title.keys holds the ticket-encrypted key
            aes128 k; aes_expand(&k, kek);
            aes_ecb_dec(&k, enc, 16);
            memcpy(s->key, enc, 16);
        } else if (ticket_title_key(s->fd, hdr + 0x230, kek, s->key)) {   // the container's own ticket, already decrypted there
        } else {
            char id[33]; static const char *hx = "0123456789abcdef";
            for (int i = 0; i < 16; i++) { id[2 * i] = hx[hdr[0x230 + i] >> 4]; id[2 * i + 1] = hx[hdr[0x230 + i] & 15]; }
            id[32] = 0;
            snprintf(err, errn, "rights id %s is neither in title.keys nor ticketed in the container", id);
            return -1;
        }
        return 0;
    }
    static const char *areas[] = {"application", "ocean", "system"};
    snprintf(kn, sizeof kn, "key_area_key_%s_%02x", areas[area < 3 ? area : 0], rev);
    uint8_t kaek[32];
    if (key_find(keys, kn, kaek, sizeof kaek) < 16) { snprintf(err, errn, "%s is missing from %s", kn, keys); return -1; }
    uint8_t area_buf[0x40];
    memcpy(area_buf, hdr + 0x300, sizeof area_buf);
    aes128 ak; aes_expand(&ak, kaek);
    aes_ecb_dec(&ak, area_buf, sizeof area_buf);
    memcpy(s->key, area_buf + 0x20, 16);
    return 0;
}
static int bktr_parse_tables(romfs_cont *c, const uint8_t *fs, char *err, size_t errn) {
    uint64_t roff = rd64(fs + 0x100), rsize = rd64(fs + 0x108), soff = rd64(fs + 0x120), ssize = rd64(fs + 0x128);
    if (memcmp(fs + 0x110, "BKTR", 4) || memcmp(fs + 0x130, "BKTR", 4)) { snprintf(err, errn, "BKTR table magic"); return -1; }
    if (!rsize || !ssize || rsize > (64u << 20) || ssize > (64u << 20) || roff > c->patch.section_size || soff > c->patch.section_size) { snprintf(err, errn, "BKTR table bounds"); return -1; }
    uint8_t *rb = malloc(rsize), *sb = malloc(ssize);
    size_t got;
    if (!rb || !sb || sect_read(c, &c->patch, roff, rb, rsize, &got) || sect_read(c, &c->patch, soff, sb, ssize, &got)) { free(rb); free(sb); snprintf(err, errn, "BKTR tables unreadable"); return -1; }
    // Relocation block: a 0x4000 header (bucket offsets) then fixed 0x4000 bucket slots, each {pad, n, end, n * {virt, phys, patch}}
    uint32_t rn = rd32(rb + 4); uint64_t total = rd64(rb + 8);
    if (!rn || rn > 0x800 || 0x4000 + (uint64_t)rn * 0x4000 > rsize) { free(rb); free(sb); snprintf(err, errn, "BKTR relocation block"); return -1; }
    size_t cap = 1024, n = 0; reloc_t *rel = malloc(cap * sizeof *rel);
    for (uint32_t b = 0; b < rn && rel; b++) {
        uint8_t *bk = rb + 0x4000 + (size_t)b * 0x4000;
        uint32_t cnt = rd32(bk + 4);
        if (cnt > 0x3FF0 / 0x14) { free(rel); rel = NULL; break; }
        for (uint32_t i = 0; i < cnt; i++) {
            if (n == cap && !(rel = realloc(rel, (cap *= 2) * sizeof *rel))) break;
            rel[n].virt = rd64(bk + 0x10 + (size_t)i * 0x14);
            rel[n].phys = rd64(bk + 0x10 + (size_t)i * 0x14 + 8);
            rel[n].patch = rd32(bk + 0x10 + (size_t)i * 0x14 + 0x10) != 0;
            n++;
        }
    }
    free(rb);
    if (!rel) { free(sb); snprintf(err, errn, "out of memory for the relocation table"); return -1; }
    // Subsection block: a 0x4000 header then fixed 0x4000 bucket slots, each {pad, n, end, n * {offset, pad, ctr}}
    uint32_t sn = rd32(sb + 4);
    if (!sn || sn > 0x800 || 0x4000 + (uint64_t)sn * 0x4000 > ssize) { free(rel); free(sb); snprintf(err, errn, "BKTR subsection block"); return -1; }
    size_t scap = 1024, m = 0; sub_t *sub = malloc(scap * sizeof *sub);
    for (uint32_t b = 0; b < sn && sub; b++) {
        uint8_t *bk = sb + 0x4000 + (size_t)b * 0x4000;
        uint32_t cnt = rd32(bk + 4);
        if (cnt > 0x3FF) { free(sub); sub = NULL; break; }
        for (uint32_t i = 0; i < cnt; i++) {
            if (m == scap && !(sub = realloc(sub, (scap *= 2) * sizeof *sub))) break;
            sub[m].off = rd64(bk + 0x10 + (size_t)i * 0x10);
            sub[m].ctr = rd32(bk + 0x10 + (size_t)i * 0x10 + 0x0C);   // {u64 offset, u32 pad, u32 ctr}: the pad is not the counter (FFR's zeros hid a wrong read here)
            m++;
        }
    }
    free(sb);
    if (!sub) { free(rel); snprintf(err, errn, "out of memory for the subsection table"); return -1; }
    // Sort and append the sentinels hactool adds: relocations end at the virtual size; the last subsection covers the
    // table region at the section counter, then everything up to the section end
    for (size_t i = 1; i < n; i++) { reloc_t x = rel[i]; size_t j = i; while (j && rel[j - 1].virt > x.virt) { rel[j] = rel[j - 1]; j--; } rel[j] = x; }
    for (size_t i = 1; i < m; i++) { sub_t x = sub[i]; size_t j = i; while (j && sub[j - 1].off > x.off) { sub[j] = sub[j - 1]; j--; } sub[j] = x; }
    rel = realloc(rel, (n + 1) * sizeof *rel); sub = realloc(sub, (m + 2) * sizeof *sub);
    if (!rel || !sub) { free(rel); free(sub); snprintf(err, errn, "out of memory for the BKTR tables"); return -1; }
    rel[n].virt = total ? total : c->patch.section_size; rel[n].phys = 0; rel[n].patch = 0; n++;
    sub[m].off = roff; sub[m].ctr = rd32(fs + 0x140); m++;
    sub[m].off = c->patch.section_size; sub[m].ctr = 0; m++;
    c->rel = rel; c->nrel = n; c->sub = sub; c->nsub = m;
    return 0;
}
static const reloc_t *reloc_at(const romfs_cont *c, uint64_t v) {
    size_t lo = 0, hi = c->nrel - 1;   // sentinel is last
    while (lo + 1 < hi) { size_t mid = (lo + hi) / 2; if (c->rel[mid].virt <= v) lo = mid; else hi = mid; }
    return c->rel + lo;
}
static const sub_t *sub_at(const romfs_cont *c, uint64_t off) {
    size_t lo = 0, hi = c->nsub - 1;
    while (lo + 1 < hi) { size_t mid = (lo + hi) / 2; if (c->sub[mid].off <= off) lo = mid; else hi = mid; }
    return c->sub + lo;
}
// Reads the merged view: virtual offsets split at relocation boundaries into patch reads (subsection counter) and base reads
static int view_read(romfs_cont *c, uint64_t v, void *buf, size_t n, size_t *got) {
    *got = 0;
    if (!c->bktr) return sect_read(c, &c->base, v, buf, n, got);
    while (*got < n) {
        uint64_t at = v + *got;
        const reloc_t *r = reloc_at(c, at);
        uint64_t next = (r + 1)->virt, end = r + 1 < c->rel + c->nrel ? next : c->patch.section_size;
        if (end <= at) { seterr(c, "BKTR relocation at %#llx has no end", (unsigned long long)at); return -1; }
        size_t chunk = n - *got < end - at ? n - *got : (size_t)(end - at);
        uint64_t P = at - r->virt + r->phys;
        int rc = 0; size_t g = 0;
        if (!r->patch) rc = sect_read(c, &c->base, P, (uint8_t *)buf + *got, chunk, &g);
        else {
            // patch data: split again at subsection boundaries (each has its own CTR value)
            while (!rc && g < chunk) {
                const sub_t *s = sub_at(c, P + g);
                uint64_t send = (s + 1)->off;
                size_t sc = chunk - g < send - (P + g) ? chunk - g : (size_t)(send - (P + g));
                size_t sg = 0;
                rc = sect_read_ex(c, &c->patch, (int64_t)s->ctr, P + g, (uint8_t *)buf + *got + g, sc, &sg);
                if (!rc && sg != sc) rc = -1;
                g += sg;
            }
        }
        if (rc || g != chunk) { if (!rc) seterr(c, "short read in the container view at %#llx", (unsigned long long)at); return -1; }
        *got += g;
    }
    return 0;
}

// ---------- mount ----------
static int idx_cmp(const void *a, const void *b) {
    const idx_ent *x = a, *y = b;
    return x->parent != y->parent ? (x->parent < y->parent ? -1 : 1) : (x->off < y->off ? -1 : x->off > y->off);
}
static int dir_ok(const romfs_cont *c, uint32_t o) { return o + 0x18 <= c->dirs_n && o + 0x18 + ((rd32(c->dirs + o + 0x14) + 3) & ~3u) <= c->dirs_n; }
static int file_ok(const romfs_cont *c, uint32_t o) { return o + 0x20 <= c->files_n && o + 0x20 + ((rd32(c->files + o + 0x1C) + 3) & ~3u) <= c->files_n; }
static int build_index(romfs_cont *c, char *err, size_t errn) {
    size_t nd = 0, nf = 0;
    for (uint32_t o = 0; o + 0x18 <= c->dirs_n; o += 0x18 + ((rd32(c->dirs + o + 0x14) + 3) & ~3u)) nd++;
    for (uint32_t o = 0; o + 0x20 <= c->files_n; o += 0x20 + ((rd32(c->files + o + 0x1C) + 3) & ~3u)) nf++;
    c->ndix = nd; c->nfix = nf;
    c->dix = malloc((nd ? nd : 1) * sizeof *c->dix);
    c->fix = malloc((nf ? nf : 1) * sizeof *c->fix);
    if (!c->dix || !c->fix) { snprintf(err, errn, "out of memory for the RomFS index"); return -1; }
    size_t i = 0;
    for (uint32_t o = 0; o + 0x18 <= c->dirs_n; o += 0x18 + ((rd32(c->dirs + o + 0x14) + 3) & ~3u)) { c->dix[i].off = o; c->dix[i].parent = rd32(c->dirs + o); i++; }
    i = 0;
    for (uint32_t o = 0; o + 0x20 <= c->files_n; o += 0x20 + ((rd32(c->files + o + 0x1C) + 3) & ~3u)) { c->fix[i].off = o; c->fix[i].parent = rd32(c->files + o); i++; }
    qsort(c->dix, c->ndix, sizeof *c->dix, idx_cmp);
    qsort(c->fix, c->nfix, sizeof *c->fix, idx_cmp);
    return 0;
}
static int read_tables(romfs_cont *c, char *err, size_t errn) {
    uint8_t rh[0x50];
    size_t got;
    if (view_read(c, c->romfs_off, rh, sizeof rh, &got) || rd64(rh) != 0x50) { snprintf(err, errn, "RomFS header"); return -1; }
    uint64_t dm = rd64(rh + 0x18), fm = rd64(rh + 0x38);
    c->dirs_n = rd64(rh + 0x20); c->files_n = rd64(rh + 0x40); c->data_off = rd64(rh + 0x48);
    if (!c->dirs_n || !c->files_n || c->dirs_n > (512u << 20) || c->files_n > (512u << 20)) { snprintf(err, errn, "RomFS table sizes %#llx/%#llx", (unsigned long long)c->dirs_n, (unsigned long long)c->files_n); return -1; }
    c->dirs = malloc(c->dirs_n); c->files = malloc(c->files_n);
    if (!c->dirs || !c->files || view_read(c, c->romfs_off + dm, c->dirs, c->dirs_n, &got) || view_read(c, c->romfs_off + fm, c->files, c->files_n, &got)) { snprintf(err, errn, "RomFS metadata tables"); return -1; }
    return 0;
}
static void cont_free(romfs_cont *c) {
    if (c->base.fd >= 0 && c->base.fd != c->patch.fd) close(c->base.fd);
    if (c->patch.fd >= 0) close(c->patch.fd);
    free(c->rel); free(c->sub);
    free(c->dirs); free(c->files); free(c->dix); free(c->fix);
    pthread_mutex_destroy(&c->lock); free(c);
}
romfs_cont *romfs_open_view(const char *base_path, const char *update_path, const char *keys, char *err, size_t errn) {
    const char *kp = keys && *keys ? keys : getenv("DM_PROD_KEYS");
    if (!kp || !*kp) { snprintf(err, errn, "no prod.keys (pass a path or set DM_PROD_KEYS)"); return NULL; }
    romfs_cont *c = calloc(1, sizeof *c);
    if (!c) { snprintf(err, errn, "out of memory"); return NULL; }
    pthread_mutex_init(&c->lock, NULL);
    c->base.fd = c->patch.fd = -1;
    char name[128] = "";
    uint64_t nb;
    c->base.fd = open(base_path, O_RDONLY);
    if (c->base.fd < 0) { snprintf(err, errn, "open %s: %s", base_path, strerror(errno)); goto fail; }
    uint8_t hkey[32];
    if (key_find(kp, "header_key", hkey, sizeof hkey) < 32) { snprintf(err, errn, "header_key is missing from %s", kp); goto fail; }
    if (container_find_nca(c->base.fd, hkey, &nb, name, sizeof name, err, errn)) goto fail;
    c->base.nca_base = nb;
    if (parse_section(c, &c->base, kp, err, errn)) goto fail;
    if (c->base.crypt == 4) { snprintf(err, errn, "the base NCA is itself a BKTR update"); goto fail; }
    if (update_path && *update_path) {
        c->patch.fd = open(update_path, O_RDONLY);
        if (c->patch.fd < 0) { snprintf(err, errn, "open %s: %s", update_path, strerror(errno)); goto fail; }
        char uname[128] = "";
        if (container_find_nca(c->patch.fd, hkey, &nb, uname, sizeof uname, err, errn)) goto fail;
        c->patch.nca_base = nb;
        if (parse_section(c, &c->patch, kp, err, errn)) goto fail;
        if (c->patch.crypt == 3) {   // a full replacement RomFS: it needs no base
            close(c->base.fd); c->base = c->patch; c->patch.fd = -1; c->patch.valid = 0;
            snprintf(name, sizeof name, "%s", uname);
        } else if (c->patch.crypt == 4) {   // a BKTR update: merged view over the base NCA
            uint8_t hdr[0xc00];
            if (pread_all(c->patch.fd, hdr, sizeof hdr, c->patch.nca_base)) { snprintf(err, errn, "update NCA header unreadable"); goto fail; }
            for (int k = 0; k < 6; k++) xts_decrypt(hkey, (uint64_t)k, hdr + k * 0x200, 0x200);
            if (!nca_header_ok(hdr)) { snprintf(err, errn, "update NCA header magic"); goto fail; }
            int rom = -1;
            for (int k = 0; k < 4; k++) if (rd32(hdr + 0x240 + k * 0x10) && hdr[0x400 + k * 0x200 + 3] == 3) { rom = k; break; }
            if (rom < 0 || c->patch.romfs_off != rd64(hdr + 0x400 + rom * 0x200 + 0x08 + 0x10 + 5 * 0x18)) { snprintf(err, errn, "update RomFS section"); goto fail; }
            if (bktr_parse_tables(c, hdr + 0x400 + rom * 0x200, err, errn)) goto fail;
            c->bktr = 1;
            snprintf(name, sizeof name, "%s + update", name);
        } else { snprintf(err, errn, "unsupported update section crypto %d", c->patch.crypt); goto fail; }
    }
    c->romfs_off = c->bktr ? c->patch.romfs_off : c->base.romfs_off;
    if (read_tables(c, err, errn) || build_index(c, err, errn)) goto fail;
    snprintf(c->err, sizeof c->err, "%s%s", name, c->bktr ? "" : "");
    return c;
fail:
    cont_free(c);
    return NULL;
}
romfs_cont *romfs_open_container(const char *path, const char *keys, char *err, size_t errn) { return romfs_open_view(path, NULL, keys, err, errn); }
void romfs_close_container(romfs_cont *c) { if (c) cont_free(c); }

// ---------- lookup / stat / read / list ----------
#define NODE_DIR 1ull
static int ent_is_dot(const char *s, uint32_t n) { return (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'); }
static void parent_range(const idx_ent *ix, size_t n, uint32_t parent, size_t *lo, size_t *hi) {
    size_t a = 0, b = n;
    while (a < b) { size_t m = (a + b) / 2; if (ix[m].parent < parent) a = m + 1; else b = m; }
    *lo = a; b = n;
    while (a < b) { size_t m = (a + b) / 2; if (ix[m].parent <= parent) a = m + 1; else b = m; }
    *hi = a;
}
static uint64_t lookup_child(romfs_cont *c, uint32_t dir, const char *name, size_t len) {
    size_t lo, hi;
    parent_range(c->dix, c->ndix, dir, &lo, &hi);
    for (size_t i = lo; i < hi; i++) {
        uint32_t o = c->dix[i].off, nl = rd32(c->dirs + o + 0x14);
        if (nl == len && !memcmp(c->dirs + o + 0x18, name, len)) return (uint64_t)o << 1 | NODE_DIR;
    }
    parent_range(c->fix, c->nfix, dir, &lo, &hi);
    for (size_t i = lo; i < hi; i++) {
        uint32_t o = c->fix[i].off, nl = rd32(c->files + o + 0x1C);
        if (nl == len && !memcmp(c->files + o + 0x20, name, len)) return (uint64_t)o << 1;
    }
    return 0;
}
uint64_t romfs_path_lookup(romfs_cont *c, const char *path) {
    uint64_t node = NODE_DIR;   // root at metadata offset 0
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > 0x300) { seterr(c, "%s: name too long", path); return 0; }
        if (!(node & NODE_DIR) || !dir_ok(c, (uint32_t)(node >> 1))) { seterr(c, "%.*s: not a directory in %s", (int)len, p, path); return 0; }
        node = lookup_child(c, (uint32_t)(node >> 1), p, len);
        if (!node) { seterr(c, "%s: not found", path); return 0; }
        p = e ? e + 1 : p + len;
    }
    return node;
}
int romfs_node_stat(romfs_cont *c, uint64_t node, int *isdir, uint64_t *size) {
    if (node & NODE_DIR) {
        if (!dir_ok(c, (uint32_t)(node >> 1))) { seterr(c, "bad directory node %#llx", (unsigned long long)node); return -1; }
        *isdir = 1; *size = 0; return 0;
    }
    uint32_t o = (uint32_t)(node >> 1);
    if (!file_ok(c, o)) { seterr(c, "bad file node %#llx", (unsigned long long)node); return -1; }
    *isdir = 0; *size = rd64(c->files + o + 0x10); return 0;
}
int romfs_node_read(romfs_cont *c, uint64_t node, uint64_t off, void *buf, size_t n, size_t *got) {
    *got = 0;
    if (node & NODE_DIR) { seterr(c, "read of a directory"); return -1; }
    uint32_t o = (uint32_t)(node >> 1);
    if (!file_ok(c, o)) { seterr(c, "bad file node %#llx", (unsigned long long)node); return -1; }
    uint64_t foff = rd64(c->files + o + 8), fsz = rd64(c->files + o + 0x10);
    if (off >= fsz) return 0;
    if (n > fsz - off) n = (size_t)(fsz - off);
    return view_read(c, c->romfs_off + c->data_off + foff + off, buf, n, got);
}
int romfs_dir_entry(romfs_cont *c, uint64_t dir, size_t index, char *name, size_t name_n, int *isdir, uint64_t *size) {
    if (!(dir & NODE_DIR) || !dir_ok(c, (uint32_t)(dir >> 1))) { seterr(c, "not a directory node"); return -1; }
    uint32_t o = (uint32_t)(dir >> 1);
    size_t lo, hi;
    parent_range(c->dix, c->ndix, o, &lo, &hi);
    for (size_t i = lo; i < hi; i++) {
        uint32_t d = c->dix[i].off, nl = rd32(c->dirs + d + 0x14);
        const char *s = (const char *)(c->dirs + d + 0x18);
        if (ent_is_dot(s, nl)) continue;
        if (index--) continue;
        if (nl >= name_n) nl = (uint32_t)name_n - 1;
        memcpy(name, s, nl); name[nl] = 0; *isdir = 1; *size = 0; return 1;
    }
    parent_range(c->fix, c->nfix, o, &lo, &hi);
    for (size_t i = lo; i < hi; i++) {
        uint32_t f = c->fix[i].off, nl = rd32(c->files + f + 0x1C);
        const char *s = (const char *)(c->files + f + 0x20);
        if (ent_is_dot(s, nl)) continue;
        if (index--) continue;
        if (nl >= name_n) nl = (uint32_t)name_n - 1;
        memcpy(name, s, nl); name[nl] = 0; *isdir = 0; *size = rd64(c->files + f + 0x10); return 1;
    }
    return 0;
}