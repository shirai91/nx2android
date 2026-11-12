// The save manager (saves_android.c): exports of the game's save in the app's external files folder (saves/), and imports from there at the next start; with
// the app's Java (SaveBridge.java), also the save file directory (a folder the player picks, mirrored) and exports and imports as .zip files anywhere
#pragma once
#include <stdint.h>
#include <time.h>

typedef struct { char name[64]; long files; long long bytes; time_t mtime; int device; } dm_save_entry;   // an export or a save folder put there
void dm_saves_paths(const char *files_root);   // saves/ under the files folder, $DM_SAVE (dm_saves_start calls it)
const char *dm_saves_dir(void);
// at the start, before the host: a pending import (saves/import.txt), then a pending export (saves/export.txt), then the save folder's save (when the app has
// one: dm_saves_bridge first). Returns 1 when the request asks the app to end
int dm_saves_start(const char *files_root);
// the committed save (and the device save) to saves/<YYYYMMDD-HHMMSS><suffix>: 0 and its name, or -errno and why (-ENOENT: the game has not saved yet)
int dm_saves_export(const char *suffix, char *name, int cap, char *err, int ecap);
int dm_saves_list(dm_save_entry *out, int max);   // the save folders in saves/, newest first
int dm_saves_request_import(const char *name);    // name is imported at the next start: 0, or -1 when the request cannot be written

// ---- through the app's Java (SaveBridge.java; none of this in a code-less app). Its pickers answer later, on a thread of Java's, in dm_saves_event ----
enum { DM_SAVES_EXPORT = 1, DM_SAVES_IMPORT, DM_SAVES_FOLDER };                                         // what answers (SaveBridge's kinds)
enum { DM_SAVES_OK, DM_SAVES_CANCELLED, DM_SAVES_LOAD, DM_SAVES_DEFAULT, DM_SAVES_FAILED = -1 };          // how: LOAD, a folder chosen whose save is to be loaded
void dm_saves_bridge(void *vm, void *activity);   // the JavaVM and the activity (ANativeActivity's), on the main thread before dm_saves_start
int dm_saves_java(void);                           // 1: the app has the bridge (the menu's save folder rows, and exports and imports as .zip files)
int dm_saves_folder_label(char *out, int cap);     // the save folder's path into out, "" when there is none (the default: the save stays in the app)
int dm_saves_folder_pick(void);                    // the folder picker (or, with a folder, the choice to change it or go back to the default): 0, -1 busy
int dm_saves_export_zip(char *err, int ecap);      // the save exported to saves/ (dm_saves_export), then the picker for the .zip: 0, or -errno and why
int dm_saves_import_zip(void);                     // the picker for a .zip; an accepted one is unpacked into saves/ and named in DM_SAVES_IMPORT's answer: 0, -1 busy
// The save folder updated from the committed save, when it changed since the last time (the player leaving or closing the game). wait 0: on a thread of its own
void dm_saves_folder_sync(int wait);
void dm_saves_event(int kind, int code, const char *message);   // front_android.c: an answer (message: the zip's name, saves/'s folder, the folder's path, or why)

// The save folder at the start: what to do with its save (folder: its fingerprint; device: the save in the app's; last: both's at the last sync, when known).
// The folder wins whenever it changed since the last sync (another device played; the save in the app is backed up first), the app's is copied to the folder
// when only it changed (the last session ended before it was synced, or the folder has no save of this game yet)
enum { DM_FOLDER_KEEP, DM_FOLDER_LOAD, DM_FOLDER_PUSH };
static inline int dm_folder_decide(int folder_has, uint64_t folder, uint64_t device, int have_last, uint64_t last) {
    if (!folder_has) return DM_FOLDER_PUSH;
    if (folder == device) return DM_FOLDER_KEEP;
    return have_last && folder == last ? DM_FOLDER_PUSH : DM_FOLDER_LOAD;
}
