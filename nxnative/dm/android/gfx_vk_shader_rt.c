// Android port, Vulkan backend: shaders the bundle lacks, translated while the game runs (DM_VK_RUNTIME_SHADERS=1, a profile's; off by default).
// NintendoWare Vfx assembles some effect programs at run time, so no game file holds them and the porter's shader split cannot find them: Pokémon Sword's opening
// needs 46 (issue #490). Until a port bundles them (the capture, gfx_vk_shader.c vk_shader_missing, and the next port), their draws were left out: the opening
// froze on black. Here a shader the bundle lacks is handed, as it is captured, to worker threads that translate it with libdmsh_xlate_rt.so (dm/android/xlate_rt:
// the porter's translator and shader_vk.py's steps, compiled for Android with NativeAOT; its output is byte for byte the bundle's). The draws that need it are left
// out until it is ready, about 1 ms a shader on an Adreno 740 device (a few frames for a burst), then it is used like a bundle shader (gfx_vk_shader.c vk_shader_q).
// Translations are kept in DM_ROOT/runtime_shaders/<library id>/<key>.dmrt ("DMRT" blobs as the library returns them) and loaded at start, so each shader is
// translated once per install of the app; a new library (another APK) starts an empty folder and the old ones are removed.
#include "gfx_vk.h"
#include "nvn_hmap.h"
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef int (*xlate_fn)(const uint8_t *ctl, uint32_t nctl, const uint8_t *code, uint32_t ncode, const char *opts, const char *workdir,
                        uint8_t **blob, uint32_t *nblob, char *log, uint32_t nlog);
static xlate_fn g_xlate; static void (*g_free)(void *);
static int g_on;
static char g_dir[1024], g_work[1024];
static nvn_hmap g_tab = {.klen = 8}, g_asked = {.klen = 8};   // key → vk_rt_ent; keys queued or translated (each is asked once)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
typedef struct rq { uint64_t key; uint8_t *ctl, *code; uint32_t nctl, ncode; struct rq *next; } rq;
static rq *g_head, *g_tail; static unsigned g_nq, g_nok, g_nfail; static double g_ms;
enum { WORKERS = 2 };

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

// a blob's entries into the table, under one lock (a geometry shader's input variants with it). The number added, -1 when the blob is malformed
static int blob_add(const uint8_t *b, uint32_t n) {
    uint32_t magic, cnt; if (n < 8) return -1; memcpy(&magic, b, 4); memcpy(&cnt, b + 4, 4);
    if (magic != 0x54524d44 || cnt == 0 || cnt > 16 || 8 + 20ull * cnt > n) return -1;   // "DMRT"
    vk_rt_ent *e = calloc(cnt, sizeof *e); uint64_t at = 8 + 20ull * cnt;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *h = b + 8 + 20 * i; memcpy(&e[i].key, h, 8); memcpy(&e[i].stage, h + 8, 4); memcpy(&e[i].nspv, h + 12, 4); memcpy(&e[i].nrefl, h + 16, 4);
        if (e[i].stage > 5 || e[i].nspv < 20 || e[i].nspv & 3 || e[i].nrefl < 20 || at + e[i].nspv + e[i].nrefl > n) { free(e); return -1; }
        uint8_t *d = malloc(e[i].nspv + e[i].nrefl); memcpy(d, b + at, e[i].nspv + e[i].nrefl); e[i].spv = d; e[i].refl = d + e[i].nspv; at += e[i].nspv + e[i].nrefl;
    }
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < cnt; i++) { nvn_hm_put(&g_tab, &e[i].key, &e[i]); nvn_hm_put(&g_asked, &e[i].key, (void *)1); }
    pthread_mutex_unlock(&g_mu);
    return (int)cnt;
}
static void rm_tree(const char *p) {   // a folder of files (an old library's translations, a worker's leftovers)
    DIR *d = opendir(p); if (!d) return; struct dirent *e; char f[1300];
    while ((e = readdir(d))) { if (e->d_name[0] == '.') continue; snprintf(f, sizeof f, "%s/%s", p, e->d_name); struct stat st;
        if (!lstat(f, &st) && S_ISDIR(st.st_mode)) rm_tree(f); else unlink(f); }
    closedir(d); rmdir(p);
}

static void *worker(void *arg) {
    char name[16]; snprintf(name, sizeof name, "vk shader rt %d", (int)(intptr_t)arg); pthread_setname_np(pthread_self(), name);
    for (;;) {
        pthread_mutex_lock(&g_mu); while (!g_head) pthread_cond_wait(&g_cv, &g_mu);
        rq *r = g_head; g_head = r->next; if (!g_head) g_tail = NULL; unsigned left = --g_nq; pthread_mutex_unlock(&g_mu);
        uint8_t *blob = NULL; uint32_t nb = 0; char log[2048] = "";
        double t0 = now_ms(); int rc = g_xlate(r->ctl, r->nctl, r->code, r->ncode, "subgroup=128", g_work, &blob, &nb, log, sizeof log); double ms = now_ms() - t0;
        int added = rc ? -1 : blob_add(blob, nb);
        if (added > 0) {   // kept for the next start: written aside, then renamed, so a killed app leaves no half file
            char path[1100], tmp[1110]; snprintf(path, sizeof path, "%s/%016llx.dmrt", g_dir, (unsigned long long)r->key); snprintf(tmp, sizeof tmp, "%s.part", path);
            FILE *f = fopen(tmp, "wb"); int ok = f && fwrite(blob, 1, nb, f) == nb; if (f && fclose(f)) ok = 0;
            if (!ok || rename(tmp, path)) { unlink(tmp); VKLOG("[RED] runtime shaders: %s could not be written (errno %d): translated again at the next start", path, errno); }
        }
        if (blob) g_free(blob);
        pthread_mutex_lock(&g_mu); unsigned k = added > 0 ? ++g_nok : (++g_nfail, g_nok); g_ms += ms; double tot = g_ms; pthread_mutex_unlock(&g_mu);
        for (char *s = log; *s; s++) if (*s == '\n') *s = ';';
        if (added > 0 && (k <= 64 || !(k & (k - 1))))
            VKLOG("runtime shaders: %016llx translated in %.1f ms (%d entries; %u done, %u queued, %.0f ms in all)%s%s", (unsigned long long)r->key, ms, added, k, left, tot, *log ? ": " : "", log);
        else if (added <= 0)
            VKLOG("[RED] runtime shaders: %016llx not translated (%s): its draws stay left out", (unsigned long long)r->key, *log ? log : rc ? "no output" : "malformed result");
        free(r->ctl); free(r->code); free(r);
    }
    return NULL;
}

static void rt_init(void) {
    const char *e = getenv("DM_VK_RUNTIME_SHADERS"); if (!(e && atoi(e))) return;
    void *h = dlopen("libdmsh_xlate_rt.so", RTLD_NOW | RTLD_LOCAL);
    if (h) { g_xlate = (xlate_fn)dlsym(h, "dmsh_rt_translate"); g_free = (void (*)(void *))dlsym(h, "dmsh_rt_free"); }
    if (!g_xlate || !g_free) { VKLOG("[RED] runtime shaders: DM_VK_RUNTIME_SHADERS is set but libdmsh_xlate_rt.so is not usable (%s): shaders the bundle lacks stay left out", h ? "no dmsh_rt_translate" : dlerror()); return; }
    // the library's identity: its file (or the APK it is read from) by size and time, which a new install changes
    Dl_info di; struct stat st; char lib[1024] = "", id[64] = "0";
    if (dladdr((void *)g_xlate, &di) && di.dli_fname) { snprintf(lib, sizeof lib, "%s", di.dli_fname); char *bang = strchr(lib, '!'); if (bang) *bang = 0; }
    if (*lib && !stat(lib, &st)) snprintf(id, sizeof id, "%llx-%llx", (unsigned long long)st.st_size, (unsigned long long)st.st_mtime);
    const char *r = getenv("DM_ROOT"); char top[1024]; snprintf(top, sizeof top, "%s/runtime_shaders", r ? r : "."); mkdir(top, 0755);
    snprintf(g_dir, sizeof g_dir, "%s/%s", top, id); snprintf(g_work, sizeof g_work, "%s/work", top);
    { DIR *d = opendir(top); struct dirent *x; char p[1300];
      while (d && (x = readdir(d))) if (x->d_name[0] != '.' && strcmp(x->d_name, id)) { snprintf(p, sizeof p, "%s/%s", top, x->d_name); rm_tree(p); }   // an older library's, and the work folder
      if (d) closedir(d); }
    mkdir(g_dir, 0755); mkdir(g_work, 0755);
    int files = 0, ents = 0; double t0 = now_ms();
    { DIR *d = opendir(g_dir); struct dirent *x; char p[1300];
      while (d && (x = readdir(d))) { size_t l = strlen(x->d_name); if (l != 21 || strcmp(x->d_name + 16, ".dmrt")) continue;
          snprintf(p, sizeof p, "%s/%s", g_dir, x->d_name); FILE *f = fopen(p, "rb"); if (!f) continue;
          fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *b = n > 0 && n < (64 << 20) ? malloc((size_t)n) : NULL;
          int k = b && fread(b, 1, (size_t)n, f) == (size_t)n ? blob_add(b, (uint32_t)n) : -1; fclose(f); free(b);
          if (k > 0) { files++; ents += k; } else { unlink(p); VKLOG("[RED] runtime shaders: %s is not a translation: removed", p); } }
      if (d) closedir(d); }
    for (int i = 0; i < WORKERS; i++) { pthread_t t; if (!pthread_create(&t, NULL, worker, (void *)(intptr_t)i)) pthread_detach(t); }
    g_on = 1;
    VKLOG("runtime shaders: on (%s, %d workers); %d translated earlier, %d entries, loaded from %s in %.1f ms", lib, WORKERS, files, ents, g_dir, now_ms() - t0);
}
int vk_rt_on(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, rt_init); return g_on; }

int vk_rt_first(uint64_t key) {
    static nvn_hmap seen = {.klen = 8};
    pthread_mutex_lock(&g_mu); int first = !nvn_hm_get(&seen, &key); if (first) nvn_hm_put(&seen, &key, (void *)1); pthread_mutex_unlock(&g_mu);
    return first;
}
const vk_rt_ent *vk_rt_find(uint64_t key) {
    if (!vk_rt_on()) return NULL;
    pthread_mutex_lock(&g_mu); const vk_rt_ent *e = nvn_hm_get(&g_tab, &key); pthread_mutex_unlock(&g_mu);
    return e;
}
// the control section and the code area as SetShaders gave them (the caller checked that they still hash to the key); copied, so the caller keeps its own
void vk_rt_request(uint64_t key, const uint8_t *ctl, uint32_t nctl, const uint8_t *code, uint32_t ncode) {
    if (!vk_rt_on()) return;
    rq *r = calloc(1, sizeof *r); r->key = key; r->nctl = nctl; r->ncode = ncode; r->ctl = malloc(nctl); r->code = malloc(ncode);
    memcpy(r->ctl, ctl, nctl); memcpy(r->code, code, ncode);
    pthread_mutex_lock(&g_mu);
    if (nvn_hm_get(&g_asked, &key)) { pthread_mutex_unlock(&g_mu); free(r->ctl); free(r->code); free(r); return; }
    nvn_hm_put(&g_asked, &key, (void *)1);
    if (g_tail) g_tail->next = r; else g_head = r; g_tail = r; g_nq++;
    pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu);
}
