// RomFS inside the player's own official container (an NSP, PFS0, or a gamecard image, XCI): mount the game's RomFS partition
// read-only without unpacking it. The data pack then holds no copy of the game; the game reads decrypted bytes straight out of the
// .nsp / .xci on storage. Layout: PFS0, or XCI (root HFS0 → secure HFS0), → NCA (XTS header, key area, AES-CTR section) → IVFC
// level 5 → RomFS metadata tables. The tables are decrypted once at mount and kept resident (Engage's are ~10 MB); file reads
// decrypt only the blocks they cover.
// A node is (offset in the metadata table) << 2 | (directory ? 1 : 2), unique within a container and never 0.
#ifndef DM_ROMFS_H
#define DM_ROMFS_H

#include <stddef.h>
#include <stdint.h>

typedef struct romfs_cont romfs_cont;

// path: .nsp or .xci (an XCZ decompressed by the porter). keys: prod.keys text, or NULL for $DM_PROD_KEYS. Returns NULL with err set.
romfs_cont *romfs_open_container(const char *path, const char *keys, char *err, size_t errn);
// The base NSP / XCI plus the player's official update NSP: a BKTR update is mounted as a view over the base RomFS; an update
// that carries a full RomFS section is used in its place. update may be NULL or empty: an integrated container (base + update +
// DLC in one XCI or NSP) is then read from the base file itself, its bundled update NCA layered over the base.
romfs_cont *romfs_open_view(const char *base_path, const char *update_path, const char *keys, char *err, size_t errn);
void romfs_close_container(romfs_cont *c);
// The add-on content bundled in a container (an integrated XCI): its indices (the title ID's low 12 bits), ascending; -1 with err set.
// Reads NCA headers only, so a load screen can ask every time.
int romfs_bundled_dlc(const char *path, const char *keys, int *out, int max, char *err, size_t errn);
// That add-on content's own RomFS (each DLC NCA holds one); NULL with err set when the container has none with that index.
romfs_cont *romfs_open_dlc(const char *path, const char *keys, int index, char *err, size_t errn);
// 0 = not found; errno-like detail goes to the container's last error (romfs_error).
uint64_t romfs_path_lookup(romfs_cont *c, const char *path);
int romfs_node_stat(romfs_cont *c, uint64_t node, int *isdir, uint64_t *size);
// Reads min(n, size - off) bytes; returns 0 on success (got may be short only at the end of the file).
int romfs_node_read(romfs_cont *c, uint64_t node, uint64_t off, void *buf, size_t n, size_t *got);
// Directory listing: index from 0; returns 1 and fills the entry, 0 at the end, -1 on error.
int romfs_dir_entry(romfs_cont *c, uint64_t dir, size_t index, char *name, size_t name_n, int *isdir, uint64_t *size);
// The resident metadata tables (bytes) and the directory / file entry counts, for the mount cache-size query
void romfs_table_usage(const romfs_cont *c, size_t *dirs_bytes, size_t *ndirs, size_t *files_bytes, size_t *nfiles);
const char *romfs_error(const romfs_cont *c);

#endif