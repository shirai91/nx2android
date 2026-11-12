// Android port: the save manager (the pause menu's Export save / Import save, and porter saves on the PC). The game's committed save ($DM_SAVE in internal
// storage, and <DM_SAVE>_device when the game mounts a device save too: hle_fs.c) is copied to the app's external files folder, which adb, porter and the PC reach
// without a permission (internal storage is out of adb's reach in a release build, which is not debuggable):
//   saves/<YYYYMMDD-HHMMSS>/          the save's files as they are: the layout a console's save tools write and read, so a console's save folder imports as it is
//   saves/<YYYYMMDD-HHMMSS>.device/   the device save, when the game has one
// The files are copied byte for byte. A save made on a console belongs to that console's user, and a game that keeps its user's id inside its own files (Animal
// Crossing: New Horizons, issue #16) reads such a save as another user's: the host's one user (hle_account.c k_uid) is not mapped into it.
// An export copies the committed tree (never <DM_SAVE>.work: a transaction in flight is not a save) into a hidden .part folder, renamed to its name once whole;
// a commit while it copies (dm_fs_commit_count moved) starts it again. An import is applied at the next start, before the host mounts anything
// (ANativeActivity_onCreate → dm_saves_start), so it never meets a commit: the menu or porter names the folder in saves/import.txt and the app ends; at the start the
// current save is exported first (<stamp>-before-import), the folder is copied to <DM_SAVE>.import and swapped with the save in one step (renameat2 RENAME_EXCHANGE),
// and the old tree is removed. saves/export.txt asks for an export at the next start (porter saves export); "quit" in it ends the app after it, without starting the
// game. Each request's outcome is saves/result.txt (KEY=VALUE lines: request, the token the request carried; result, ok or error; name, backup or error)
// With the app's Java (SaveBridge.java: porter builds it when it can, the APK is code-less otherwise and all of what follows is off), the menu's Export save
// and Import save go through the system's pickers instead: an export is made in saves/ as above, then zipped where the player says as <title ID>/<files>; an
// import is a .zip with a folder named after this game's title ID (another game's save is refused), unpacked into saves/ and imported as above. And the save
// file directory, a folder the player picks (the grant to it is all the permission asked for): the committed save is mirrored into <folder>/<title ID>/ when
// the player leaves or closes the game (dm_saves_folder_sync), and read back at the start (folder_start, before the host mounts the save), so the folder can
// be shared with a PC or a cloud sync app. saves/folder-sync.txt keeps the save's fingerprint at the last sync, which tells a folder another device changed
// (it wins: the app's save is exported to saves/<stamp>-before-folder first) from a session that ended before its sync (the app's save goes to the folder)
#include "dm.h"
#include "saves_android.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

uint64_t dm_fs_commit_count(void);   // hle_fs.c: commits so far
static char g_dir[600], g_save[600], g_dev[620];   // saves/, the save, the device save

static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int has_entries(const char *p) {
    DIR *d = opendir(p); struct dirent *e; int n = 0; if (!d) return 0;
    while (!n && (e = readdir(d))) n = strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    closedir(d); return n;
}
static void rm_tree(const char *p) {
    struct stat st; if (lstat(p, &st)) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p); struct dirent *e;
        while (d && (e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char c[1200]; snprintf(c, sizeof c, "%s/%s", p, e->d_name); rm_tree(c); }
        if (d) closedir(d);
        rmdir(p);
    } else unlink(p);
}
static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY | O_CLOEXEC); if (in < 0) return -errno;
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0660);   // (EEXIST: two names that differ only by case, on the case-insensitive external storage)
    if (out < 0) { int e = errno; close(in); return -e; }
    char buf[1 << 16]; ssize_t n; int r = 0;
    while (!r && (n = read(in, buf, sizeof buf)) > 0) for (ssize_t o = 0; o < n && !r;) { ssize_t w = write(out, buf + o, (size_t)(n - o)); if (w < 0) r = -errno; else o += w; }
    if (!r && n < 0) r = -errno;
    if (!r && fsync(out)) r = -errno;
    close(in); if (close(out) && !r) r = -errno;
    return r;
}
// src's tree into dst (made here: it must not exist); *files and *bytes add up what was copied; the first failure's -errno, its path in err
static int copy_tree(const char *src, const char *dst, long *files, long long *bytes, char *err, int ecap) {
    if (mkdir(dst, 0770) && errno != EEXIST) { int e = errno; snprintf(err, (size_t)ecap, "%s", dst); return -e; }
    DIR *d = opendir(src); if (!d) { int e = errno; snprintf(err, (size_t)ecap, "%s", src); return -e; }
    struct dirent *e; int r = 0;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[1200], b[1200]; struct stat st; snprintf(a, sizeof a, "%s/%s", src, e->d_name); snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
        if (lstat(a, &st)) { r = -errno; snprintf(err, (size_t)ecap, "%s", a); }
        else if (S_ISDIR(st.st_mode)) r = copy_tree(a, b, files, bytes, err, ecap);
        else if (S_ISREG(st.st_mode)) { if ((r = copy_file(a, b))) snprintf(err, (size_t)ecap, "%s", b); else { ++*files; *bytes += st.st_size; } }
    }
    closedir(d); return r;
}
static void tree_size(const char *p, long *files, long long *bytes) {
    DIR *d = opendir(p); struct dirent *e; if (!d) return;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[1200]; struct stat st; snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        if (lstat(c, &st)) continue;
        if (S_ISDIR(st.st_mode)) tree_size(c, files, bytes); else if (S_ISREG(st.st_mode)) { ++*files; *bytes += st.st_size; }
    }
    closedir(d);
}
static int swap_in(const char *neu, const char *dst) {   // neu takes dst's place in one step; the old tree goes
    if (!is_dir(dst)) return rename(neu, dst) ? -errno : 0;
    if (syscall(SYS_renameat2, AT_FDCWD, neu, AT_FDCWD, dst, 2 /* RENAME_EXCHANGE */)) return -errno;
    rm_tree(neu); return 0;
}
static void result(const char *req, const char *fmt, ...) {   // saves/result.txt: this request's outcome (also the log's)
    char body[700]; va_list ap; va_start(ap, fmt); vsnprintf(body, sizeof body, fmt, ap); va_end(ap);
    char p[640], tmp[650]; snprintf(p, sizeof p, "%s/result.txt", g_dir); snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "w"); if (f) { fprintf(f, "request=%s\n%s\n", req, body); fclose(f); rename(tmp, p); }
    for (char *s = body; *s; s++) if (*s == '\n') *s = ' ';
    LOG("%s[saves] %s", strstr(body, "result=error") ? "[RED] " : "", body);
}

void dm_saves_paths(const char *files_root) {
    snprintf(g_dir, sizeof g_dir, "%s/saves", files_root); mkdir(g_dir, 0770);
    const char *s = getenv("DM_SAVE"); snprintf(g_save, sizeof g_save, "%s", s ? s : "");
    size_t L = strlen(g_save); while (L > 1 && g_save[L - 1] == '/') g_save[--L] = 0;   // as hle_fs.c strips it before appending _device
    snprintf(g_dev, sizeof g_dev, "%s_device", g_save);
}
const char *dm_saves_dir(void) { return g_dir; }

// The committed save (and the device save, dev) copied to part (and part.device), as it is between two commits
static int snapshot(const char *part, int dev, long *files, long long *bytes, char *err, int ecap) {
    char partd[720]; snprintf(partd, sizeof partd, "%s.device", part); int r = -EAGAIN;
    for (int attempt = 0; attempt < 3 && r == -EAGAIN; attempt++) {   // a commit swaps the tree being copied: copy it again
        uint64_t c0 = dm_fs_commit_count(); *files = 0; *bytes = 0; err[0] = 0;
        rm_tree(part); rm_tree(partd);
        r = copy_tree(g_save, part, files, bytes, err, ecap);
        if (!r && dev) r = copy_tree(g_dev, partd, files, bytes, err, ecap);
        if (dm_fs_commit_count() != c0) r = -EAGAIN;
    }
    return r;
}
int dm_saves_export(const char *suffix, char *name, int cap, char *err, int ecap) {
    err[0] = 0;
    if (!g_save[0]) { snprintf(err, (size_t)ecap, "DM_SAVE is not set"); return -EINVAL; }
    int dev = is_dir(g_dev) && has_entries(g_dev);
    if (!has_entries(g_save) && !dev) { snprintf(err, (size_t)ecap, "the game has not saved yet"); return -ENOENT; }
    mkdir(g_dir, 0770);
    char stamp[64], fin[700]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(name, (size_t)cap, "%s%s", stamp, suffix);
    for (int k = 2; snprintf(fin, sizeof fin, "%s/%s", g_dir, name), is_dir(fin) && k < 100; k++) snprintf(name, (size_t)cap, "%s%s-%d", stamp, suffix, k);
    char part[700], partd[720], find[720]; snprintf(part, sizeof part, "%s/.%s.part", g_dir, name); snprintf(partd, sizeof partd, "%s.device", part); snprintf(find, sizeof find, "%s.device", fin);
    long files = 0; long long bytes = 0; int r = snapshot(part, dev, &files, &bytes, err, ecap);
    if (!r && dev && rename(partd, find)) { r = -errno; snprintf(err, (size_t)ecap, "%s", find); }
    if (!r && rename(part, fin)) { r = -errno; snprintf(err, (size_t)ecap, "%s", fin); rm_tree(find); }   // the save's own folder last: once there, the export is whole
    if (r) { rm_tree(part); rm_tree(partd);
        if (r == -EAGAIN) snprintf(err, (size_t)ecap, "the game kept saving while it was copied");
        else { char e2[400]; snprintf(e2, sizeof e2, "%s (%s)", err, strerror(-r)); snprintf(err, (size_t)ecap, "%s", e2); }
        LOG("[RED] [saves] export failed: %s", err); return r; }
    LOG("[saves] exported the save to %s: %ld files, %lld bytes%s", fin, files, bytes, dev ? " (with the device save)" : "");
    return 0;
}

static int by_newest(const void *a, const void *b) {
    const dm_save_entry *x = a, *y = b;
    return x->mtime != y->mtime ? (x->mtime < y->mtime ? 1 : -1) : strcmp(y->name, x->name);
}
int dm_saves_list(dm_save_entry *out, int max) {
    DIR *d = opendir(g_dir); struct dirent *e; int n = 0; if (!d) return 0;
    while (n < max && (e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (e->d_name[0] == '.' || L >= sizeof out->name || (L > 7 && !strcmp(e->d_name + L - 7, ".device"))) continue;   // hidden (.part), and device saves go with theirs
        char p[1200]; struct stat st; snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
        if (stat(p, &st) || !S_ISDIR(st.st_mode)) continue;
        dm_save_entry *s = &out[n]; memset(s, 0, sizeof *s); memcpy(s->name, e->d_name, L + 1); s->mtime = st.st_mtime;
        tree_size(p, &s->files, &s->bytes);
        char dp[1220]; snprintf(dp, sizeof dp, "%s.device", p); if ((s->device = is_dir(dp))) tree_size(dp, &s->files, &s->bytes);
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, by_newest);
    return n;
}

int dm_saves_request_import(const char *name) {
    char p[640], tmp[650]; snprintf(p, sizeof p, "%s/import.txt", g_dir); snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "w"); if (!f) { LOG("[RED] [saves] cannot write %s (errno %d)", tmp, errno); return -1; }
    fprintf(f, "%s\nrequest=menu\n", name); fclose(f);
    if (rename(tmp, p)) { LOG("[RED] [saves] cannot write %s (errno %d)", p, errno); return -1; }
    LOG("[saves] %s will be imported at the next start", name); return 0;
}

// src (and src.device) takes the save's place (and the device save's), the current save exported first as saves/<stamp><suffix> so it can be taken back (bk:
// its name, "" when the game had none): 0, or -1 and why in err. what: src for the messages
static int swap_save(const char *src, const char *suffix, const char *what, char *bk, int bkcap, long *files, long long *bytes, char *err, int ecap) {
    char srcd[720], e2[600] = ""; snprintf(srcd, sizeof srcd, "%s.device", src); *files = 0; *bytes = 0;
    int r = dm_saves_export(suffix, bk, bkcap, e2, sizeof e2);
    if (r && r != -ENOENT) { snprintf(err, (size_t)ecap, "the current save could not be backed up first (%s): nothing was imported", e2); return -1; }
    if (r) bk[0] = 0;
    const char *from[2] = {src, srcd}, *to[2] = {g_save, g_dev}; char tmp[2][640];
    for (int k = 0; k < 2; k++) {   // both copies first, then both swaps
        snprintf(tmp[k], sizeof tmp[k], "%s.import", to[k]); rm_tree(tmp[k]);
        if (k == 1 && !is_dir(srcd)) continue;   // no device save in the folder: the game's own stays
        if ((r = copy_tree(from[k], tmp[k], files, bytes, e2, sizeof e2))) { rm_tree(tmp[0]); rm_tree(tmp[1]);
            snprintf(err, (size_t)ecap, "copying %s failed: %s (%s); the save is unchanged", what, e2, strerror(-r)); return -1; }
    }
    for (int k = 0; k < 2; k++) if (is_dir(tmp[k]) && (r = swap_in(tmp[k], to[k]))) {
        snprintf(err, (size_t)ecap, "swapping in %s failed (%s)%s", to[k], strerror(-r), k ? "; the save itself was imported" : ": the save is unchanged"); return -1; }
    return 0;
}
static int import_apply(const char *name, const char *req) {   // 1: imported
    char src[700], srcd[720], err[700] = "", bk[96] = ""; snprintf(src, sizeof src, "%s/%s", g_dir, name); snprintf(srcd, sizeof srcd, "%s.device", src);
    if (!name[0] || strchr(name, '/') || name[0] == '.' || !is_dir(src)) { result(req, "result=error\nerror=no save folder %s in %s", name, g_dir); return 0; }
    if (!has_entries(src) && !is_dir(srcd)) { result(req, "result=error\nerror=%s is empty", name); return 0; }
    long files = 0; long long bytes = 0;
    if (swap_save(src, "-before-import", name, bk, sizeof bk, &files, &bytes, err, sizeof err)) { result(req, "result=error\nerror=%s", err); return 0; }
    result(req, "result=ok\nname=%s\nbackup=%s\nfiles=%ld\nbytes=%lld", name, bk, files, bytes);
    return 1;
}

// ---- a save's fingerprint: FNV-1a over its tree's names, kinds and bytes in name order (not its times: a copy has the same) ----
static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; while (n--) { h ^= *b++; h *= 0x100000001b3ull; } return h; }
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static uint64_t tree_hash(const char *p, uint64_t h) {
    DIR *d = opendir(p); if (!d) return h;
    char **names = NULL; size_t n = 0, cap = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) { char **m = realloc(names, (cap = cap ? cap * 2 : 16) * sizeof *names); if (!m) break; names = m; }
        if ((names[n] = strdup(e->d_name))) n++;
    }
    closedir(d);
    if (n) qsort(names, n, sizeof *names, by_name);
    for (size_t i = 0; i < n; i++) {
        char c[1200]; struct stat st; snprintf(c, sizeof c, "%s/%s", p, names[i]);
        h = fnv(h, names[i], strlen(names[i]) + 1);
        if (lstat(c, &st)) h = fnv(h, "?", 1);
        else if (S_ISDIR(st.st_mode)) { h = fnv(h, "/", 1); h = tree_hash(c, h); h = fnv(h, "\\", 1); }
        else if (S_ISREG(st.st_mode)) {
            uint64_t sz = (uint64_t)st.st_size; h = fnv(h, &sz, sizeof sz);
            int fd = open(c, O_RDONLY | O_CLOEXEC); char buf[1 << 16]; ssize_t k;
            if (fd >= 0) { while ((k = read(fd, buf, sizeof buf)) > 0) h = fnv(h, buf, (size_t)k); close(fd); }
        }
        free(names[i]);
    }
    free(names); return h;
}
static uint64_t save_hash(const char *save, const char *dev) {   // a save and its device save (one with no files is none)
    uint64_t h = tree_hash(save, 0xcbf29ce484222325ull);
    if (is_dir(dev) && has_entries(dev)) { h = fnv(h, "\0device", 7); h = tree_hash(dev, h); }
    return h;
}
static int last_read(uint64_t *h) {   // saves/folder-sync.txt: the fingerprint of the save the folder had at the last sync
    char p[640]; unsigned long long v; snprintf(p, sizeof p, "%s/folder-sync.txt", g_dir);
    FILE *f = fopen(p, "r"); int ok = f && fscanf(f, "hash=%llx", &v) == 1; if (f) fclose(f);
    if (ok) *h = v; return ok;
}
static void last_write(uint64_t h) {
    char p[640], tmp[650]; snprintf(p, sizeof p, "%s/folder-sync.txt", g_dir); snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "w"); if (f) { fprintf(f, "hash=%016llx\n", (unsigned long long)h); fclose(f); rename(tmp, p); }
}
static void last_forget(void) { char p[640]; snprintf(p, sizeof p, "%s/folder-sync.txt", g_dir); unlink(p); }

// ---- the app's Java (SaveBridge.java), when it has it: GameActivity.saveBridge() ----
static JavaVM *g_vm; static jobject g_bridge;   // NULL: a code-less app
static jmethodID g_m_label, g_m_pick, g_m_export, g_m_import, g_m_restore, g_m_mirror;
static pthread_mutex_t g_label_mu = PTHREAD_MUTEX_INITIALIZER; static char g_label[512];   // the save folder's path, "" none
static JNIEnv *jattach(int *detach) {
    JNIEnv *E = NULL; *detach = 0; if (!g_vm) return NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&E, JNI_VERSION_1_6) == JNI_OK) return E;
    if ((*g_vm)->AttachCurrentThread(g_vm, &E, NULL) != JNI_OK) return NULL;
    *detach = 1; return E;
}
static void jdetach(int detach) { if (detach) (*g_vm)->DetachCurrentThread(g_vm); }
static int jfail(JNIEnv *E) { if (!(*E)->ExceptionCheck(E)) return 0; (*E)->ExceptionDescribe(E); (*E)->ExceptionClear(E); return 1; }   // (logcat has the Java trace)
static void label_refresh(JNIEnv *E) {
    char buf[512] = ""; jstring s = (*E)->CallObjectMethod(E, g_bridge, g_m_label);
    if (!jfail(E) && s) { const char *c = (*E)->GetStringUTFChars(E, s, NULL); if (c) { snprintf(buf, sizeof buf, "%s", c); (*E)->ReleaseStringUTFChars(E, s, c); } }
    if (s) (*E)->DeleteLocalRef(E, s);
    pthread_mutex_lock(&g_label_mu); memcpy(g_label, buf, sizeof buf); pthread_mutex_unlock(&g_label_mu);
}
void dm_saves_bridge(void *vm, void *activity) {
    g_vm = vm; int dt; JNIEnv *E = jattach(&dt); if (!E) return;
    jclass c = (*E)->GetObjectClass(E, (jobject)activity); jmethodID m = c ? (*E)->GetMethodID(E, c, "saveBridge", "()Lorg/nx2a/SaveBridge;") : NULL;
    if (jfail(E) || !m) { LOG("[saves] a code-less app: the menu's own save manager (saves/ in the files folder), no save folder"); if (c) (*E)->DeleteLocalRef(E, c); jdetach(dt); return; }
    jobject b = (*E)->CallObjectMethod(E, (jobject)activity, m); jclass bc = b && !jfail(E) ? (*E)->GetObjectClass(E, b) : NULL;
    if (bc) {
        g_m_label = (*E)->GetMethodID(E, bc, "folderLabel", "()Ljava/lang/String;"); g_m_pick = (*E)->GetMethodID(E, bc, "pickFolder", "(Ljava/lang/String;)Z");
        g_m_export = (*E)->GetMethodID(E, bc, "exportZip", "(Ljava/lang/String;Ljava/lang/String;)Z"); g_m_import = (*E)->GetMethodID(E, bc, "importZip", "(Ljava/lang/String;Ljava/lang/String;)Z");
        g_m_restore = (*E)->GetMethodID(E, bc, "restore", "(Ljava/lang/String;Ljava/lang/String;)I"); g_m_mirror = (*E)->GetMethodID(E, bc, "mirror", "(Ljava/lang/String;Ljava/lang/String;)I");
    }
    if (jfail(E) || !bc || !g_m_label || !g_m_pick || !g_m_export || !g_m_import || !g_m_restore || !g_m_mirror)
        LOG("[RED] [saves] the app's SaveBridge does not answer as this host expects (an APK built from another checkout?): no save folder, the menu's own save manager");
    else { g_bridge = (*E)->NewGlobalRef(E, b); label_refresh(E); LOG("[saves] the Java save bridge is there: save folder %s", g_label[0] ? g_label : "none (the default: the save stays in the app)"); }
    if (bc) (*E)->DeleteLocalRef(E, bc); if (b) (*E)->DeleteLocalRef(E, b); (*E)->DeleteLocalRef(E, c);
    jdetach(dt);
}
int dm_saves_java(void) { return g_bridge != NULL; }
int dm_saves_folder_label(char *out, int cap) { pthread_mutex_lock(&g_label_mu); snprintf(out, (size_t)cap, "%s", g_label); pthread_mutex_unlock(&g_label_mu); return (int)strlen(out); }
static int jcall(jmethodID m, int boolean, const char *a, const char *b, int dflt) {   // a bridge method taking one or two strings: its boolean (0, 1) or int; dflt on failure
    if (!g_bridge) return dflt; int dt; JNIEnv *E = jattach(&dt); if (!E) return dflt;
    jstring x = (*E)->NewStringUTF(E, a), y = b ? (*E)->NewStringUTF(E, b) : NULL; int r = dflt;
    if (x && (!b || y) && !jfail(E)) {
        if (boolean) { jboolean v = b ? (*E)->CallBooleanMethod(E, g_bridge, m, x, y) : (*E)->CallBooleanMethod(E, g_bridge, m, x); if (!jfail(E)) r = v ? 1 : 0; }
        else { jint v = (*E)->CallIntMethod(E, g_bridge, m, x, y); if (!jfail(E)) r = v; }
    }
    if (x) (*E)->DeleteLocalRef(E, x); if (y) (*E)->DeleteLocalRef(E, y);
    jdetach(dt); return r;
}
int dm_saves_folder_pick(void) { return jcall(g_m_pick, 1, DM_GAME_ID, NULL, 0) == 1 ? 0 : -1; }
int dm_saves_import_zip(void) { return jcall(g_m_import, 1, g_dir, DM_GAME_ID, 0) == 1 ? 0 : -1; }
int dm_saves_export_zip(char *err, int ecap) {   // the export in saves/ stays there, as the menu's own export's does
    char name[96], p[700]; int r = dm_saves_export("", name, sizeof name, err, ecap); if (r) return r;
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    if (jcall(g_m_export, 1, p, DM_GAME_ID, 0) != 1) { snprintf(err, (size_t)ecap, "the file picker could not be opened"); return -EBUSY; }
    LOG("[saves] the save is in saves/%s: the picker for its zip is open", name); return 0;
}

// ---- the save folder: mirrored when the player leaves or closes the game, read back at the start. g_sync_mu orders the syncs ----
static pthread_mutex_t g_sync_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_synced;                       // dm_fs_commit_count at the last sync: no commit since, nothing to copy
static int g_sync_dirty = 1, g_sync_hold = 1;   // the folder may be behind the app's save; no syncs: not before the folder was read at this start (folder_start),
                                                // a read that failed, or the folder's save to be loaded at the next start
static int folder_push(const char *why) {   // the committed save into the folder: 0, -1 failed (g_sync_mu held)
    int dev = is_dir(g_dev) && has_entries(g_dev);
    if (!has_entries(g_save) && !dev) { LOG("[saves] save folder: the game has not saved yet: nothing to copy (%s)", why); return 0; }   // never an empty save over the folder's
    char stage[640], staged[660], err[600], label[512]; long files = 0; long long bytes = 0;
    snprintf(stage, sizeof stage, "%s/.folder-out", g_dir); snprintf(staged, sizeof staged, "%s.device", stage); dm_saves_folder_label(label, sizeof label);
    int r = snapshot(stage, dev, &files, &bytes, err, sizeof err);
    if (r) { LOG("[RED] [saves] save folder: copying the save failed (%s: %s)", err, r == -EAGAIN ? "the game kept saving" : strerror(-r)); rm_tree(stage); rm_tree(staged); return -1; }
    uint64_t h = save_hash(stage, staged), last;
    if (last_read(&last) && last == h) { rm_tree(stage); rm_tree(staged); LOG("[saves] save folder %s: up to date (%s)", label, why); return 0; }
    r = jcall(g_m_mirror, 0, DM_GAME_ID, stage, -1);
    rm_tree(stage); rm_tree(staged);
    if (r) { LOG("[RED] [saves] save folder %s: writing the save failed (%s)", label, r == -2 ? "the app has no access to it any more: choose it again in the menu"
                 : r == -3 ? "no folder" : "logcat has why"); return -1; }
    last_write(h); LOG("[saves] save folder: the save copied to %s/%s (%ld files, %lld bytes; %s)", label, DM_GAME_ID, files, bytes, why); return 0;
}
static void *sync_run(void *why) {
    pthread_mutex_lock(&g_sync_mu);
    uint64_t c = dm_fs_commit_count(); char label[512]; dm_saves_folder_label(label, sizeof label);
    if (label[0] && !g_sync_hold && (g_sync_dirty || c != g_synced) && !folder_push(why)) { g_synced = c; g_sync_dirty = 0; }
    pthread_mutex_unlock(&g_sync_mu); return NULL;
}
// On a thread of its own either way: the callers (the main thread's onPause and onDestroy, the host's thread at its end) may not be where Java can attach
void dm_saves_folder_sync(int wait) {
    if (!g_bridge) return;
    pthread_t t; if (pthread_create(&t, NULL, sync_run, wait ? "the game closed" : "the player left the game")) { LOG("[RED] [saves] save folder: cannot start the sync"); return; }
    if (wait) pthread_join(t, NULL); else pthread_detach(t);
}
static void folder_start(int imported) {   // at the start, before the host mounts the save
    char label[512]; if (!g_bridge || !dm_saves_folder_label(label, sizeof label)) return;
    pthread_mutex_lock(&g_sync_mu);
    char stage[640], staged[660]; snprintf(stage, sizeof stage, "%s/.folder-in", g_dir); snprintf(staged, sizeof staged, "%s.device", stage);
    if (imported) {   // the import replaced the save: the folder gets it (else the folder's older save would win at the next start)
        LOG("[saves] save folder %s: an import was applied: it is copied there", label); last_forget();
        if (!folder_push("an import")) g_sync_dirty = 0;
        g_sync_hold = 0; pthread_mutex_unlock(&g_sync_mu); return;
    }
    rm_tree(stage); rm_tree(staged);
    int r = jcall(g_m_restore, 0, DM_GAME_ID, stage, -1);
    if (r < 0) LOG("[RED] [saves] save folder %s could not be read (%s): the game starts with the save in the app", label, r == -2 ? "the app has no access to it any more: choose it again in the menu" : "logcat has why");
    else {
        uint64_t hd = save_hash(g_save, g_dev), hf = r ? save_hash(stage, staged) : 0, last = 0; int have = last_read(&last);
        int d = dm_folder_decide(r == 1, hf, hd, have, last);
        if (d == DM_FOLDER_KEEP) { last_write(hd); g_sync_dirty = 0; g_sync_hold = 0; LOG("[saves] save folder %s: the same save as the app's", label); }
        else if (d == DM_FOLDER_PUSH) {
            LOG("[saves] save folder %s: %s", label, r ? "the save in the app is newer (its last session ended before it was copied): it goes to the folder" : "no save of this game there yet: the app's goes there");
            if (!folder_push("the start")) g_sync_dirty = 0;
            g_sync_hold = 0;
        } else {
            char bk[96] = "", err[700]; long files = 0; long long bytes = 0;
            if (swap_save(stage, "-before-folder", "the folder's save", bk, sizeof bk, &files, &bytes, err, sizeof err)) LOG("[RED] [saves] save folder %s: loading its save failed: %s", label, err);
            else { last_write(hf); g_sync_dirty = 0; g_sync_hold = 0; LOG("[saves] save folder %s: its save loaded (%ld files, %lld bytes)%s%s", label, files, bytes, bk[0] ? "; the app's save was kept as saves/" : "", bk); }
        }
    }
    rm_tree(stage); rm_tree(staged);
    pthread_mutex_unlock(&g_sync_mu);
}
// SaveBridge's answers, on a thread of Java's
JNIEXPORT void JNICALL Java_org_nx2a_SaveBridge_done(JNIEnv *E, jclass cls, jint kind, jint code, jbyteArray message) {
    (void)cls; char m[600] = "";
    if (message) { jsize n = (*E)->GetArrayLength(E, message); if (n > (jsize)sizeof m - 1) n = (jsize)sizeof m - 1; (*E)->GetByteArrayRegion(E, message, 0, n, (jbyte *)m); m[n] = 0; jfail(E); }
    if (kind == DM_SAVES_FOLDER && (code == DM_SAVES_OK || code == DM_SAVES_LOAD || code == DM_SAVES_DEFAULT)) {
        if (g_bridge) label_refresh(E);
        pthread_mutex_lock(&g_sync_mu); last_forget(); g_sync_dirty = 1; g_sync_hold = code == DM_SAVES_LOAD; pthread_mutex_unlock(&g_sync_mu);
        LOG("[saves] save folder: %s%s", code == DM_SAVES_DEFAULT ? "none (the default: the save stays in the app)" : m, code == DM_SAVES_LOAD ? ": its save is loaded at the next start" : "");
        if (code == DM_SAVES_OK) dm_saves_folder_sync(0);   // the app's save goes there now
    } else LOG("[saves] %s: %s%s%s", kind == DM_SAVES_EXPORT ? "export" : kind == DM_SAVES_IMPORT ? "import" : "save folder",
               code == DM_SAVES_OK ? "done" : code == DM_SAVES_CANCELLED ? "cancelled" : "failed", m[0] ? ": " : "", m);
    dm_saves_event(kind, code, m);
}
static int read_request(const char *file, char *first, int fcap, char *req, int rcap, int *quit) {   // saves/<file>: its first line, its request= token; removed
    char p[640], line[256]; snprintf(p, sizeof p, "%s/%s", g_dir, file);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    first[0] = 0; snprintf(req, (size_t)rcap, "-"); *quit = 0;
    for (int i = 0; fgets(line, sizeof line, f); i++) {
        char *e = line + strlen(line); while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (!strncmp(line, "request=", 8)) snprintf(req, (size_t)rcap, "%s", line + 8);
        else if (!strcmp(line, "quit")) *quit = 1;
        else if (!first[0]) snprintf(first, (size_t)fcap, "%s", line);
    }
    fclose(f); unlink(p); return 1;
}
int dm_saves_start(const char *files_root) {
    dm_saves_paths(files_root);
    char name[256], req[96]; int quit = 0, q, imported = 0;
    if (read_request("import.txt", name, sizeof name, req, sizeof req, &q)) { quit |= q; imported = import_apply(name, req); }
    if (read_request("export.txt", name, sizeof name, req, sizeof req, &q)) {
        quit |= q; char out[96], err[600]; int r = dm_saves_export("", out, sizeof out, err, sizeof err);
        if (r) result(req, "result=error\nerror=%s", err); else result(req, "result=ok\nname=%s", out);
    }
    if (!quit) folder_start(imported);   // (a request that ends the app leaves the folder alone: the game does not run)
    return quit;
}
