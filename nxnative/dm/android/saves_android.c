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
#include "dm.h"
#include "saves_android.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
    long files = 0; long long bytes = 0; int r = -EAGAIN;
    for (int attempt = 0; attempt < 3 && r == -EAGAIN; attempt++) {   // a commit swaps the tree being copied: copy it again
        uint64_t c0 = dm_fs_commit_count(); files = 0; bytes = 0; err[0] = 0;
        rm_tree(part); rm_tree(partd);
        r = copy_tree(g_save, part, &files, &bytes, err, ecap);
        if (!r && dev) r = copy_tree(g_dev, partd, &files, &bytes, err, ecap);
        if (dm_fs_commit_count() != c0) r = -EAGAIN;
    }
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

static void import_apply(const char *name, const char *req) {
    char src[700], srcd[720], err[600] = "", bk[96] = ""; snprintf(src, sizeof src, "%s/%s", g_dir, name); snprintf(srcd, sizeof srcd, "%s.device", src);
    if (!name[0] || strchr(name, '/') || name[0] == '.' || !is_dir(src)) { result(req, "result=error\nerror=no save folder %s in %s", name, g_dir); return; }
    if (!has_entries(src) && !is_dir(srcd)) { result(req, "result=error\nerror=%s is empty", name); return; }
    int r = dm_saves_export("-before-import", bk, sizeof bk, err, sizeof err);   // the current save first, so an import can be taken back
    if (r && r != -ENOENT) { result(req, "result=error\nerror=the current save could not be backed up first (%s): nothing was imported", err); return; }
    if (r) bk[0] = 0;
    const char *from[2] = {src, srcd}, *to[2] = {g_save, g_dev}; char tmp[2][640]; long files = 0; long long bytes = 0;
    for (int k = 0; k < 2; k++) {   // both copies first, then both swaps
        snprintf(tmp[k], sizeof tmp[k], "%s.import", to[k]); rm_tree(tmp[k]);
        if (k == 1 && !is_dir(srcd)) continue;   // no device save in the folder: the game's own stays
        if ((r = copy_tree(from[k], tmp[k], &files, &bytes, err, sizeof err))) { rm_tree(tmp[0]); rm_tree(tmp[1]);
            result(req, "result=error\nerror=copying %s failed: %s (%s); the save is unchanged", name, err, strerror(-r)); return; }
    }
    for (int k = 0; k < 2; k++) if (is_dir(tmp[k]) && (r = swap_in(tmp[k], to[k]))) {
        result(req, "result=error\nerror=swapping in %s failed (%s)%s", to[k], strerror(-r), k ? "; the save itself was imported" : ": the save is unchanged"); return; }
    result(req, "result=ok\nname=%s\nbackup=%s\nfiles=%ld\nbytes=%lld", name, bk, files, bytes);
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
    char name[256], req[96]; int quit = 0, q;
    if (read_request("import.txt", name, sizeof name, req, sizeof req, &q)) { quit |= q; import_apply(name, req); }
    if (read_request("export.txt", name, sizeof name, req, sizeof req, &q)) {
        quit |= q; char out[96], err[600]; int r = dm_saves_export("", out, sizeof out, err, sizeof err);
        if (r) result(req, "result=error\nerror=%s", err); else result(req, "result=ok\nname=%s", out);
    }
    return quit;
}
