// The game data's zip: written front to back (a picked document need not seek), each file deflated whole in memory (a save is small); read from memory. A Mac's Finder
// files (.DS_Store, ._*, __MACOSX/) are left out both ways
#include "data_zip.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

static int junk(const char *n) { return !strcmp(n, ".") || !strcmp(n, "..") || !strcmp(n, ".DS_Store") || !strncmp(n, "._", 2); }
static uint8_t *le16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return p + 2; }
static uint8_t *le32(uint8_t *p, uint32_t v) { p = le16(p, v & 0xffff); return le16(p, v >> 16); }
static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

typedef struct { int fd, err; unsigned n, time, date; uint64_t off; uint8_t *cd; size_t cdn, cdcap; } zipw;
static void zw_out(zipw *z, const void *b, size_t n) {
    for (const uint8_t *p = b; n && !z->err; ) { ssize_t w = write(z->fd, p, n); if (w < 0 && errno == EINTR) continue; if (w <= 0) { z->err = 1; break; } p += w; n -= (size_t)w; z->off += (uint64_t)w; }
}
static void zw_cd(zipw *z, const uint8_t *b, size_t n) {
    if (z->cdn + n > z->cdcap) { size_t c = z->cdcap * 2 + n + 4096; uint8_t *q = realloc(z->cd, c); if (!q) { z->err = 1; return; } z->cd = q; z->cdcap = c; }
    memcpy(z->cd + z->cdn, b, n); z->cdn += n;
}
static void zw_file(zipw *z, const char *name, const uint8_t *d, size_t n) {   // raw deflate (method 8), stored when that is no smaller
    uLong cap = compressBound((uLong)n) + 64; uint8_t *c = malloc(cap); size_t cn = 0; int method = 0;
    z_stream s; memset(&s, 0, sizeof s);
    if (c && deflateInit2(&s, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK) {
        s.next_in = (Bytef *)d; s.avail_in = (uInt)n; s.next_out = c; s.avail_out = (uInt)cap;
        if (deflate(&s, Z_FINISH) == Z_STREAM_END && s.total_out < n) { cn = s.total_out; method = 8; }
        deflateEnd(&s);
    }
    uint32_t crc = (uint32_t)crc32(0, d, (uInt)n), at = (uint32_t)z->off, sz = (uint32_t)(method ? cn : n); unsigned nl = (unsigned)strlen(name);
    uint8_t h[46], *p = le32(h, 0x04034b50);   // version 2.0, UTF-8 names
    p = le16(p, 20); p = le16(p, 0x800); p = le16(p, method); p = le16(p, z->time); p = le16(p, z->date); p = le32(p, crc); p = le32(p, sz); p = le32(p, (uint32_t)n); p = le16(p, nl); p = le16(p, 0);
    zw_out(z, h, 30); zw_out(z, name, nl); zw_out(z, method ? c : d, sz); free(c);
    p = le32(h, 0x02014b50);   // made on Unix, a 0644 file
    p = le16(p, 0x0314); p = le16(p, 20); p = le16(p, 0x800); p = le16(p, method); p = le16(p, z->time); p = le16(p, z->date); p = le32(p, crc); p = le32(p, sz); p = le32(p, (uint32_t)n);
    p = le16(p, nl); p = le16(p, 0); p = le16(p, 0); p = le16(p, 0); p = le16(p, 0); p = le32(p, 0100644u << 16); p = le32(p, at);
    zw_cd(z, h, 46); zw_cd(z, (const uint8_t *)name, nl); z->n++;
}
static void zw_tree(zipw *z, const char *dir, const char *pre) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e; char p[1024], q[640];
    while (!z->err && (e = readdir(d))) {
        if (junk(e->d_name)) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name); snprintf(q, sizeof q, "%s/%s", pre, e->d_name); struct stat st;
        if (stat(p, &st)) { z->err = 1; break; }
        if (S_ISDIR(st.st_mode)) { zw_tree(z, p, q); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        int fd = open(p, O_RDONLY | O_CLOEXEC); uint8_t *b = malloc((size_t)st.st_size + 1); size_t n = 0; ssize_t r = 0;
        while (fd >= 0 && b && n < (size_t)st.st_size && (r = read(fd, b + n, (size_t)st.st_size - n)) > 0) n += (size_t)r;
        if (fd < 0 || !b || r < 0 || n != (size_t)st.st_size) z->err = 1; else zw_file(z, q, b, n);
        if (fd >= 0) close(fd);
        free(b);
    }
    closedir(d);
}
int dz_write(int fd, const char *const dirs[], const char *const names[], int n, unsigned *files) {
    zipw z; memset(&z, 0, sizeof z); z.fd = fd;
    time_t now = time(NULL); struct tm t; localtime_r(&now, &t);
    z.time = (unsigned)(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2); z.date = (unsigned)((t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday);
    for (int k = 0; k < n; k++) zw_tree(&z, dirs[k], names[k]);
    uint8_t e[22], *p = le32(e, 0x06054b50); uint64_t cdoff = z.off;
    zw_out(&z, z.cd, z.cdn);
    p = le16(p, 0); p = le16(p, 0); p = le16(p, z.n); p = le16(p, z.n); p = le32(p, (uint32_t)z.cdn); p = le32(p, (uint32_t)cdoff); p = le16(p, 0);
    zw_out(&z, e, 22); free(z.cd);
    if (files) *files = z.n;
    return z.err ? -1 : 0;
}

static int mkdirs(char *path) {   // the folders above path's last part
    for (char *s = strchr(path + 1, '/'); s; s = strchr(s + 1, '/')) { *s = 0; int r = mkdir(path, 0700); *s = '/'; if (r && errno != EEXIST) return -1; }
    return 0;
}
int dz_extract(const uint8_t *b, size_t n, const char *const dest[2]) {
    size_t eo = 0, cd = 0, ne = 0; int files = 0, bad = 0, root = 0; char path[1200], name[600];
    if (n >= 22) for (size_t i = n - 22 + 1; i-- > (n > 22 + 65535 ? n - 22 - 65535 : 0); ) if (rd32(b + i) == 0x06054b50) { eo = i + 1; break; }   // eo: the end record + 1
    if (!eo) return -1;
    ne = rd16(b + eo - 1 + 10); cd = rd32(b + eo - 1 + 16);
    if (cd > n) return -1;
    for (int pass = 0; pass < 2 && !bad; pass++) {   // pass 0: whether a file is at the top (then no folder is taken off)
        size_t p = cd;
        for (size_t i = 0; i < ne && !bad; i++) {
            if (p + 46 > n || rd32(b + p) != 0x02014b50) { bad = 1; break; }
            unsigned method = rd16(b + p + 10), nl = rd16(b + p + 28); uint32_t crc = rd32(b + p + 16), cs = rd32(b + p + 20), us = rd32(b + p + 24), lo = rd32(b + p + 42);
            if (p + 46 + nl > n || nl >= sizeof name) { bad = 1; break; }
            memcpy(name, b + p + 46, nl); name[nl] = 0; p += 46 + nl + rd16(b + p + 30) + rd16(b + p + 32);
            const char *base = strrchr(name, '/'); base = base ? base + 1 : name;
            if (!nl || name[nl - 1] == '/' || !strncmp(name, "__MACOSX/", 9) || junk(base)) continue;
            if (name[0] == '/' || strstr(name, "..") || strchr(name, '\\')) { bad = 1; break; }
            if (pass == 0) { root |= !strchr(name, '/'); continue; }
            int k = 0; const char *rel = name;
            if (!root) { const char *sl = strchr(name, '/'); k = sl - name >= 7 && !strncmp(sl - 7, "_device", 7); rel = sl + 1; }
            snprintf(path, sizeof path, "%s/%s", dest[k], rel);
            if ((size_t)lo + 30 > n || rd32(b + lo) != 0x04034b50) { bad = 1; break; }
            size_t at = (size_t)lo + 30 + rd16(b + lo + 26) + rd16(b + lo + 28);
            if (at + cs > n || (method != 0 && method != 8) || (method == 0 && cs != us)) { bad = 1; break; }
            uint8_t *out = method ? malloc((size_t)us + 1) : (uint8_t *)b + at;
            if (method) {
                z_stream s; memset(&s, 0, sizeof s); s.next_in = (Bytef *)b + at; s.avail_in = cs; s.next_out = out; s.avail_out = us + 1;
                if (!out || inflateInit2(&s, -15) != Z_OK) bad = 1;
                else { if (inflate(&s, Z_FINISH) != Z_STREAM_END || s.total_out != us) bad = 1; inflateEnd(&s); }
            }
            if (!bad && (uint32_t)crc32(0, out, us) != crc) bad = 1;
            int o = bad || mkdirs(path) ? -1 : open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            for (size_t w = 0; o >= 0 && w < us; ) { ssize_t q = write(o, out + w, us - w); if (q <= 0) { bad = 1; break; } w += (size_t)q; }
            if (o < 0 || fsync(o)) bad = 1;
            if (o >= 0 && close(o)) bad = 1;
            if (method) free(out);
            files += !bad;
        }
    }
    return bad ? -1 : files;
}
