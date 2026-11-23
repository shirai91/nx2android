// Shared-font ownership and LoadFont forwarding through the real forced registration; no game module or font file is needed.
#include "../hle_sdk16.c"
#include <mach-o/getsect.h>
#include <sys/wait.h>
#include <sys/resource.h>

static int checks, fails, calls, lookups, missing;
static void *seen_self;
static char *seen_name;
static const void *seen_data;
static unsigned seen_index, seen_capacity;
static uint32_t status;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static uint32_t original(void *self, char *name, const void *data, unsigned index, unsigned capacity) {
    calls++; seen_self = self; seen_name = name; seen_data = data;
    seen_index = index; seen_capacity = capacity;
    return status;
}
void *guest_sym(const char *name) {
    lookups++;
    CHECK(!strcmp(name, "_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj"), "original symbol lookup");
    return missing ? NULL : original;
}
module_t g_mod[MAX_MOD];
int g_nmod;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
int dm_sdk_major(void) { return 20; }
int dm_main_imports(const char *prefix) { (void)prefix; return 1; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void hle_bind_system_event(void *ev, int autoclear) { (void)ev; (void)autoclear; }
void hle_signal_system_event(void *ev) { (void)ev; }
uint32_t hle_hid_connected_npad(void) { return 0; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }
extern const struct mach_header_64 _mh_execute_header;
// A firmware file as the console stores it: the decoded header (marker, then the TTF length big-endian) + the TTF, XORed per little-endian word with 0x06186249 (out has n + 8 bytes)
static void firmware_bfttf(uint8_t *out, const uint8_t *ttf, size_t n) {
    static const uint8_t key[4] = {0x49, 0x62, 0x18, 0x06};
    uint32_t done = 0x18029a7fu; memcpy(out, &done, 4);
    out[4] = (uint8_t)(n >> 24); out[5] = (uint8_t)(n >> 16); out[6] = (uint8_t)(n >> 8); out[7] = (uint8_t)n;
    memcpy(out + 8, ttf, n);
    for (size_t i = 0; i < n + 8; i++) out[i] ^= key[i & 3];
}
static void write_file(const char *path, const void *b, size_t n) { FILE *f = fopen(path, "wb"); if (f) { fwrite(b, 1, n, f); fclose(f); } }
static int face_is(font_t f, const uint8_t *ttf, size_t n) { return f.p && f.n == n + 8 && !memcmp(f.p + 8, ttf, n); }
static void child_reset(void) { checks = 0; fails = 0; }
// font_get caches for the process, so each case runs in its own child. The files are already in DM_SYSDATA
static void regional_present(const uint8_t *cn, const uint8_t *cx, const uint8_t *tw, const uint8_t *ko, const uint8_t *stdt, const uint8_t *ext, size_t n, size_t ext_n,
                             uint32_t (*load)(void *, char *, const void *, unsigned, unsigned)) {
    child_reset(); unsetenv("DM_SHARED_FONT");
    font_t a = font_get(1), b = font_get(2), c = font_get(3), d = font_get(4), s = font_get(0), e = font_get(5);
    CHECK(face_is(a, cn, n) && face_is(b, cx, n) && face_is(c, tw, n) && face_is(d, ko, n), "types 1-4 are the regional firmware fonts");
    CHECK(face_is(s, stdt, n) && a.p != s.p && b.p != s.p, "type 0 stays the standard font");
    CHECK(face_is(e, ext, ext_n) && e.p != s.p, "type 5 stays the extension font");
    int self; char name[8] = {0}; status = 0;
    CHECK(load(&self, name, a.p, 3, 4) == 0 && seen_data == a.p + 8, "LoadFont skips the header of a regional font");
    _exit(fails != 0);
}
static void regional_player(const uint8_t *pa, size_t n) {
    child_reset();
    font_t s = font_get(0), a = font_get(1), b = font_get(2), c = font_get(3), d = font_get(4);
    CHECK(face_is(s, pa, n) && a.p == s.p && b.p == s.p && c.p == s.p && d.p == s.p, "DM_SHARED_FONT wins types 0-4 over the regional firmware fonts");
    _exit(fails != 0);
}
static void regional_missing(const uint8_t *stdt, size_t n) {
    child_reset(); unsetenv("DM_SHARED_FONT");
    font_t s = font_get(0), a = font_get(1), b = font_get(2), c = font_get(3), d = font_get(4);
    CHECK(face_is(s, stdt, n) && a.p == s.p && b.p == s.p && c.p == s.p && d.p == s.p, "a missing regional font is the standard font");
    _exit(fails != 0);
}
int main(void) {
    unsigned long bytes = 0;
    const hle_force_ent *e = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    uint32_t (*load)(void *, char *, const void *, unsigned, unsigned) = NULL;
    unsigned count = 0;
    for (size_t i = 0; e && i < bytes / sizeof *e; i++) {
        if (strcmp(e[i].name, "_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj")) continue;
        load = e[i].fn; count++;
        CHECK(e[i].why && e[i].why[0], "takeover has a reason");
    }
    CHECK(count == 1 && load, "exact LoadFont forced registration exists once");
    if (!load) return 1;
    // A missing SDK implementation must fail loudly rather than fabricate a successful font load.
    pid_t child = fork();
    CHECK(child >= 0, "missing-original child starts");
    if (!child) { struct rlimit r = {0, 0}; setrlimit(RLIMIT_CORE, &r); missing = 1; load(NULL, NULL, NULL, 0, 0); _exit(1); }
    if (child > 0) { int s; waitpid(child, &s, 0); CHECK(WIFSIGNALED(s) && WTERMSIG(s) == SIGABRT, "missing original aborts"); }
    uint8_t standard[64] = {0}, extension[64] = {0}, unrelated[64] = {0};
    uint32_t marker = 0x18029a7fu;
    memcpy(standard, &marker, 4); memcpy(extension, &marker, 4); memcpy(unrelated, &marker, 4);
    char name[50] = {0}; int self;
    const void *before[] = {NULL, unrelated, standard, extension};
    for (unsigned i = 0; i < sizeof before / sizeof *before; i++) {
        status = 403; int n = calls;
        CHECK(load(&self, name, before[i], 17, sizeof name) == status, "uninitialized fonts preserve result");
        CHECK(calls == n + 1 && seen_data == before[i], "uninitialized fonts leave all data unchanged");
    }
    std_f = (font_t){standard, sizeof standard}; ext_f = (font_t){extension, sizeof extension};
    const void *input[] = {standard, extension, standard + 8, extension + 8, standard + 1, extension + 9, unrelated, NULL};
    const void *expect[] = {standard + 8, extension + 8, standard + 8, extension + 8, standard + 1, extension + 9, unrelated, NULL};
    const uint32_t results[] = {0, 403, 301, 0x80000001u, ~0u};
    for (unsigned r = 0; r < sizeof results / sizeof *results; r++) for (unsigned i = 0; i < sizeof input / sizeof *input; i++) {
        int n = calls; status = results[r];
        CHECK(load(&self, name, input[i], ~0u, sizeof name) == status, "SDK status preserved");
        CHECK(calls == n + 1 && seen_data == expect[i], "only exact host bases skip the header, once");
        CHECK(seen_self == &self && seen_name == name && seen_index == ~0u && seen_capacity == sizeof name, "all other arguments preserved");
    }
    ext_f = std_f;
    CHECK(load(&self, name, standard, 0, 0) == status && seen_data == standard + 8, "extension fallback to standard");
    CHECK(seen_index == 0 && seen_capacity == 0, "zero unsigned arguments preserved");
    CHECK(lookups == 1, "original resolved once");
    CHECK(!memcmp(standard, unrelated, sizeof standard) && !memcmp(extension, unrelated, sizeof extension), "input bytes unchanged");
    // The console's own fonts from system data (DM_SYSDATA/<title>/<file>): the firmware's obfuscated BFTTF is decoded and served as the BFTTF layout (marker, length, TTF)
    char root[] = "/tmp/t_shared_font.XXXXXX";
    CHECK(mkdtemp(root) != NULL, "system data folder");
    char std_dir[1100], ext_dir[1100], cn_dir[1100], tw_dir[1100], ko_dir[1100], std_file[1200], ext_file[1200], pa_file[1200];
    char cn_org[1200], cn_ext[1200], tw_file[1200], ko_file[1200];
    snprintf(std_dir, sizeof std_dir, "%s/0100000000000811", root); snprintf(ext_dir, sizeof ext_dir, "%s/0100000000000810", root);
    snprintf(cn_dir, sizeof cn_dir, "%s/0100000000000814", root); snprintf(tw_dir, sizeof tw_dir, "%s/0100000000000813", root); snprintf(ko_dir, sizeof ko_dir, "%s/0100000000000812", root);
    snprintf(std_file, sizeof std_file, "%s/nintendo_udsg-r_std_003.bfttf", std_dir); snprintf(ext_file, sizeof ext_file, "%s/nintendo_ext_003.bfttf", ext_dir);
    snprintf(cn_org, sizeof cn_org, "%s/nintendo_udsg-r_org_zh-cn_003.bfttf", cn_dir); snprintf(cn_ext, sizeof cn_ext, "%s/nintendo_udsg-r_ext_zh-cn_003.bfttf", cn_dir);
    snprintf(tw_file, sizeof tw_file, "%s/nintendo_udjxh-db_zh-tw_003.bfttf", tw_dir); snprintf(ko_file, sizeof ko_file, "%s/nintendo_udsg-r_ko_003.bfttf", ko_dir);
    snprintf(pa_file, sizeof pa_file, "%s/pa.ttf", root);
    mkdir(std_dir, 0700); mkdir(ext_dir, 0700); mkdir(cn_dir, 0700); mkdir(tw_dir, 0700); mkdir(ko_dir, 0700);
    uint8_t ttf[16] = {0, 1, 0, 0, 'S', 'T', 'D', '1', 1, 2, 3, 4, 5, 6, 7, 8}, pa[16] = {0, 1, 0, 0, 'P', 'A', 'D', 'D', 1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t cn[16] = {0, 1, 0, 0, 'C', 'N', 'O', 'R', 1, 2, 3, 4, 5, 6, 7, 8}, cx[16] = {0, 1, 0, 0, 'C', 'N', 'E', 'X', 1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t tw[16] = {0, 1, 0, 0, 'T', 'W', '0', '1', 1, 2, 3, 4, 5, 6, 7, 8}, ko[16] = {0, 1, 0, 0, 'K', 'O', '0', '1', 1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t ext[20] = {'O', 'T', 'T', 'O', 'E', 'X', 'T', '1', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, junk[16] = {'N', 'O', 'P', 'E', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t fw[24], fw_ext[28], fw_bad[24], fw_cn[24], fw_cx[24], fw_tw[24], fw_ko[24];
    firmware_bfttf(fw, ttf, sizeof ttf); write_file(std_file, fw, sizeof fw); write_file(pa_file, pa, sizeof pa);
    firmware_bfttf(fw_cn, cn, sizeof cn); firmware_bfttf(fw_cx, cx, sizeof cx); firmware_bfttf(fw_tw, tw, sizeof tw); firmware_bfttf(fw_ko, ko, sizeof ko);
    write_file(cn_org, fw_cn, sizeof fw_cn); write_file(cn_ext, fw_cx, sizeof fw_cx); write_file(tw_file, fw_tw, sizeof fw_tw); write_file(ko_file, fw_ko, sizeof fw_ko);
    firmware_bfttf(fw_bad, junk, sizeof junk); write_file(ext_file, fw_bad, sizeof fw_bad);
    setenv("DM_SYSDATA", root, 1); unsetenv("DM_SHARED_FONT"); unsetenv("DM_SHARED_FONT_EXT"); unsetenv("DM_FONT_DIR");
    uint32_t decoded_marker = 0x18029a7fu, len16 = __builtin_bswap32(16u ^ 0x49621806u);
    font_t sd = font_sysdata(0x0100000000000811ull, "nintendo_udsg-r_std_003.bfttf");
    CHECK(sd.p && sd.n == 24, "system data font is the BFTTF size of its TTF");
    CHECK(sd.p && !memcmp(sd.p, &decoded_marker, 4) && !memcmp(sd.p + 4, &len16, 4), "system data font carries the decoded marker and length");
    CHECK(sd.p && !memcmp(sd.p + 8, ttf, sizeof ttf), "system data font payload is the plain TTF");
    CHECK(font_sysdata(0x0100000000000810ull, "nintendo_ext_003.bfttf").p == NULL, "a file that is not a BFTTF is refused");
    firmware_bfttf(fw_ext, ext, sizeof ext); write_file(ext_file, fw_ext, sizeof fw_ext);
    font_t se = font_sysdata(0x0100000000000810ull, "nintendo_ext_003.bfttf");
    CHECK(se.p && se.n == 28 && !memcmp(se.p + 8, ext, sizeof ext), "extension font from system data decodes an OpenType payload");
    CHECK(font_sysdata(0x0100000000000802ull, "nintendo_udsg-r_std_003.bfttf").p == NULL, "a title or file that is not there is none");
    unsetenv("DM_SYSDATA");
    CHECK(font_sysdata(0x0100000000000811ull, "nintendo_udsg-r_std_003.bfttf").p == NULL, "no DM_SYSDATA is none");
    setenv("DM_SYSDATA", root, 1);
    // Each font_get cache is one-shot, so the regional cases run in children before this process calls font_get
    pid_t reg = fork();
    CHECK(reg >= 0, "regional font child starts");
    if (!reg) regional_present(cn, cx, tw, ko, ttf, ext, sizeof ttf, sizeof ext, load);
    if (reg > 0) { int s; waitpid(reg, &s, 0); CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 0, "regional fonts from system data"); }
    setenv("DM_SHARED_FONT", pa_file, 1);
    reg = fork();
    CHECK(reg >= 0, "player font child starts");
    if (!reg) regional_player(pa, sizeof pa);
    if (reg > 0) { int s; waitpid(reg, &s, 0); CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 0, "DM_SHARED_FONT covers types 0-4"); }
    unsetenv("DM_SHARED_FONT");
    unlink(cn_org); unlink(cn_ext); unlink(tw_file); unlink(ko_file);
    reg = fork();
    CHECK(reg >= 0, "missing regional font child starts");
    if (!reg) regional_missing(ttf, sizeof ttf);
    if (reg > 0) { int s; waitpid(reg, &s, 0); CHECK(WIFEXITED(s) && WEXITSTATUS(s) == 0, "missing regional fonts fall back to the standard font"); }
    // Order: DM_SHARED_FONT (the player's choice) wins the standard font; the system data's extension font wins over DM_SHARED_FONT_EXT (porter's stand-in)
    setenv("DM_SHARED_FONT", pa_file, 1); setenv("DM_SHARED_FONT_EXT", pa_file, 1);
    font_t fs = font_get(0), fe = font_get(5);
    CHECK(fs.p && fs.n == 24 && !memcmp(fs.p + 8, pa, sizeof pa), "DM_SHARED_FONT wins over system data for the standard font");
    CHECK(fe.p && fe.n == 28 && !memcmp(fe.p + 8, ext, sizeof ext), "system data extension font wins over DM_SHARED_FONT_EXT");
    unlink(pa_file); unlink(std_file); unlink(ext_file); unlink(cn_org); unlink(cn_ext); unlink(tw_file); unlink(ko_file);
    rmdir(std_dir); rmdir(ext_dir); rmdir(cn_dir); rmdir(tw_dir); rmdir(ko_dir); rmdir(root);
    unsetenv("DM_SHARED_FONT"); unsetenv("DM_SHARED_FONT_EXT"); unsetenv("DM_SYSDATA");
    printf("shared font: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
