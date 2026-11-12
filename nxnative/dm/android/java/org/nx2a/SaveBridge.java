package org.nx2a;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * The save manager's Android side (saves_android.c calls it through JNI): the system's own pickers (the Storage Access Framework), so the app asks for no
 * storage permission: picking a folder grants that folder alone, and the grant is kept (takePersistableUriPermission) across starts.
 *   Save file directory: a folder the player picks; the game's save is mirrored into <folder>/<title ID>/ (and <title ID>.device/) when the player leaves or
 *   closes the game, and read back from there at the next start (restore, mirror). The files are written in place, as ordinary files on the device's storage,
 *   so a sync app watching the folder (FolderSync's instant sync) sees each changed file, and an unchanged file is not written again.
 *   Export save: a .zip the player names and places, holding <title ID>/<the save's files>. Import save: such a .zip, refused when it holds no folder named
 *   after this game's title ID; accepted ones are unpacked into the files folder's saves/ and imported at the next start, as a save folder put there is.
 * Native calls arrive on its own threads; the pickers and dialogs run on the UI thread, the copying on a thread of its own, and each request ends in exactly one
 * done(kind, code, message).
 */
public final class SaveBridge {
    static { System.loadLibrary("dmhost"); }
    static final int EXPORT = 1, IMPORT = 2, FOLDER = 3;                   // done()'s kinds (saves_android.h DM_SAVES_*)
    static final int OK = 0, CANCELLED = 1, LOAD = 2, DEFAULT = 3, FAILED = -1;   // its codes: LOAD, the folder chosen and its save to be loaded; DEFAULT, back to no folder
    private static native void done(int kind, int code, byte[] message);
    private static final int REQUEST = 0x5a00;   // startActivityForResult's codes: REQUEST + kind
    private static final String PREFS = "nx2a.saves", KEY = "folder", STORAGE = "com.android.externalstorage.documents";
    private final Activity activity;
    private int pending;                       // the kind of the request under way, 0 none (the UI thread, or under this)
    private String title = "", dir = "";       // the request's title ID, and its folder (Export: the copy to zip; Import: saves/)

    SaveBridge(Activity activity) { this.activity = activity; }

    // ---- called from native code ----
    /** The save folder as a path (what the menu shows), "" when there is none. A folder whose grant is gone (revoked, or the folder deleted) says so. */
    public synchronized String folderLabel() {
        Uri tree = saved(); if (tree == null) return "";
        return (granted(tree) ? "" : "No access: ") + label(tree);
    }
    public synchronized boolean pickFolder(String titleId) {
        if (!start(FOLDER, titleId, "")) return false;
        activity.runOnUiThread(() -> {
            Uri cur = saved();
            if (cur == null) { openTree(null); return; }
            new AlertDialog.Builder(activity).setTitle("Save file directory").setMessage(label(cur) + "\n\nThe game's save is copied here when you leave or close the game, and "
                    + "loaded from here when it starts.")
                .setPositiveButton("Choose another folder", (d, b) -> openTree(cur))
                .setNeutralButton("Use default", (d, b) -> { forget(); finish(FOLDER, DEFAULT, ""); })
                .setNegativeButton("Cancel", (d, b) -> finish(FOLDER, CANCELLED, ""))
                .setOnCancelListener(d -> finish(FOLDER, CANCELLED, "")).show();
        });
        return true;
    }
    /** folder: the save as exported to saves/ (saves_android.c dm_saves_export), zipped where the player says */
    public synchronized boolean exportZip(String folder, String titleId) {
        if (!start(EXPORT, titleId, folder)) return false;
        activity.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/zip")
                .putExtra(Intent.EXTRA_TITLE, appName() + " save data - " + new SimpleDateFormat("yyyy-MM-dd HH_mm", Locale.US).format(new Date()) + ".zip");
            launch(i, EXPORT);
        });
        return true;
    }
    /** saves: the files folder's saves/, where an accepted zip is unpacked */
    public synchronized boolean importZip(String saves, String titleId) {
        if (!start(IMPORT, titleId, saves)) return false;
        activity.runOnUiThread(() -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                .putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/x-zip", "application/octet-stream"});
            launch(i, IMPORT);
        });
        return true;
    }
    /** The folder's <title ID>/ (and .device/) copied into stage (and stage.device): 1, 0 when the folder has no save of this game, -1 failed, -2 no access */
    public int restore(String titleId, String stage) {
        Uri tree = saved(); if (tree == null) return 0;
        try {
            ContentResolver cr = activity.getContentResolver();
            Map<String, Doc> top = children(cr, tree, DocumentsContract.getTreeDocumentId(tree));
            Doc save = find(top, titleId), dev = find(top, titleId + ".device");
            if (save == null || !save.dir) return 0;
            download(cr, tree, save.id, new File(stage));
            if (dev != null && dev.dir) download(cr, tree, dev.id, new File(stage + ".device"));
            return 1;
        } catch (SecurityException e) { return -2; }
        catch (IOException | RuntimeException e) { android.util.Log.e("dmhost", "[saves] reading the save folder failed", e); return -1; }
    }
    /** src (and src.device) mirrored into the folder's <title ID>/ (and .device/): 0, -1 failed, -2 no access, -3 no folder */
    public int mirror(String titleId, String src) {
        Uri tree = saved(); if (tree == null) return -3;
        try {
            ContentResolver cr = activity.getContentResolver(); String root = DocumentsContract.getTreeDocumentId(tree);
            Map<String, Doc> top = children(cr, tree, root);
            Doc save = find(top, titleId), dev = find(top, titleId + ".device");
            upload(cr, tree, new File(src), save != null && save.dir ? save.id : mkdir(cr, tree, root, titleId));
            File local = new File(src + ".device");
            if (local.isDirectory()) upload(cr, tree, local, dev != null && dev.dir ? dev.id : mkdir(cr, tree, root, titleId + ".device"));
            else if (dev != null) DocumentsContract.deleteDocument(cr, DocumentsContract.buildDocumentUriUsingTree(tree, dev.id));
            return 0;
        } catch (SecurityException e) { return -2; }
        catch (IOException | RuntimeException e) { android.util.Log.e("dmhost", "[saves] writing the save folder failed", e); return -1; }
    }

    // ---- the activity's results (GameActivity, ExternalActivity forward them) ----
    boolean onActivityResult(int request, int result, Intent data) {
        int kind = request - REQUEST; if (kind != EXPORT && kind != IMPORT && kind != FOLDER) return false;
        Uri uri = result == Activity.RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) { finish(kind, CANCELLED, ""); return true; }
        if (kind == FOLDER) folderChosen(uri, data.getFlags());
        else if (kind == EXPORT) work(() -> exportTo(uri));
        else work(() -> importFrom(uri));
        return true;
    }

    // ---- the save folder ----
    private void openTree(Uri initial) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        if (initial != null) i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, initial);
        launch(i, FOLDER);
    }
    private void folderChosen(Uri tree, int flags) {
        ContentResolver cr = activity.getContentResolver();
        try { cr.takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION); }
        catch (SecurityException e) { fail(FOLDER, "Android did not let the game keep access to this folder. Choose another one.", e); return; }
        work(() -> {
            Doc have;
            try { have = find(children(cr, tree, DocumentsContract.getTreeDocumentId(tree)), title); }
            catch (IOException | RuntimeException e) { release(tree); fail(FOLDER, "The folder could not be read: " + e.getMessage(), e); return; }
            String where = label(tree);
            if (have == null) { activity.runOnUiThread(() -> { keep(tree); finish(FOLDER, OK, where); }); return; }
            activity.runOnUiThread(() -> new AlertDialog.Builder(activity).setTitle("This folder has a save for this game")
                .setMessage(where + "/" + title + "\n\nLoad it: the game closes, and the folder's save replaces the save on this device when you open the game again "
                    + "(the save on this device is backed up first, in the app's saves folder).\n\nReplace it: the save on this device is copied over the folder's.")
                .setPositiveButton("Load it", (d, b) -> { keep(tree); finish(FOLDER, LOAD, where); })
                .setNeutralButton("Replace it", (d, b) -> { keep(tree); finish(FOLDER, OK, where); })
                .setNegativeButton("Cancel", (d, b) -> { release(tree); finish(FOLDER, CANCELLED, ""); })
                .setOnCancelListener(d -> { release(tree); finish(FOLDER, CANCELLED, ""); }).show());
        });
    }
    private SharedPreferences prefs() { return activity.getSharedPreferences(PREFS, Activity.MODE_PRIVATE); }
    private synchronized Uri saved() { String s = prefs().getString(KEY, null); return s == null ? null : Uri.parse(s); }
    private synchronized void keep(Uri tree) {   // the new folder; the old one's grant goes
        Uri old = saved(); prefs().edit().putString(KEY, tree.toString()).commit();
        if (old != null && !old.equals(tree)) release(old);
    }
    private synchronized void forget() { Uri old = saved(); prefs().edit().remove(KEY).commit(); if (old != null) release(old); }
    private void release(Uri tree) {
        if (tree.equals(saved())) return;
        try { activity.getContentResolver().releasePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION); }
        catch (SecurityException ignored) {}
    }
    private boolean granted(Uri tree) {
        for (UriPermission p : activity.getContentResolver().getPersistedUriPermissions()) if (p.getUri().equals(tree) && p.isReadPermission() && p.isWritePermission()) return true;
        return false;
    }
    /** A tree as a path: the device's storage as Android names it (/storage/emulated/0/...), any other provider by its name */
    static String label(Uri tree) {
        String id; try { id = DocumentsContract.getTreeDocumentId(tree); } catch (RuntimeException e) { return tree.toString(); }
        String path = STORAGE.equals(tree.getAuthority()) ? storagePath(id) : null;
        return path != null ? path : tree.getAuthority() + ": " + id;
    }
    /** The device storage's document id ("primary:Saves/Switch") as a path */
    private static String storagePath(String id) {
        int c = id.indexOf(':'); String vol = c < 0 ? id : id.substring(0, c), path = c < 0 ? "" : id.substring(c + 1);
        String base = vol.equals("primary") ? "/storage/emulated/0" : vol.equals("home") ? "/storage/emulated/0/Documents" : "/storage/" + vol;
        return path.isEmpty() ? base : base + "/" + path;
    }
    /** The folder a document the save dialog made is in, as a path; null when its provider does not say (the Downloads provider's own ids, a cloud's) */
    private static String folderOf(Uri doc) {
        String id; try { id = DocumentsContract.getDocumentId(doc); } catch (RuntimeException e) { return null; }
        String path = STORAGE.equals(doc.getAuthority()) ? storagePath(id) : id.startsWith("raw:") ? id.substring(4) : null;
        int slash = path == null ? -1 : path.lastIndexOf('/');
        return slash > 0 ? path.substring(0, slash) : null;
    }

    // ---- export and import ----
    private void exportTo(Uri zip) {
        ContentResolver cr = activity.getContentResolver(); int files = 0;
        try (OutputStream raw = cr.openOutputStream(zip, "wt"); ZipOutputStream out = new ZipOutputStream(raw)) {
            if (raw == null) throw new IOException("the file could not be opened");
            files += zipTree(out, new File(dir), title + "/");
            File dev = new File(dir + ".device"); if (dev.isDirectory()) files += zipTree(out, dev, title + ".device/");
        } catch (IOException | RuntimeException e) {
            try { DocumentsContract.deleteDocument(cr, zip); } catch (Exception ignored) {}
            fail(EXPORT, "The zip file could not be written: " + e.getMessage(), e); return;
        }
        android.util.Log.i("dmhost", "[saves] exported " + files + " files to " + zip);
        String shown = name(cr, zip), where = folderOf(zip);
        activity.runOnUiThread(() -> android.widget.Toast.makeText(activity, where != null ? "Save exported to " + where : "Save exported: " + shown,   // (a toast shows
            android.widget.Toast.LENGTH_LONG).show());                                                                                           // two lines at most)
        finish(EXPORT, OK, shown);
    }
    private static int zipTree(ZipOutputStream out, File dir, String prefix) throws IOException {
        out.putNextEntry(new ZipEntry(prefix)); out.closeEntry(); int n = 0;
        File[] list = dir.listFiles(); if (list == null) return 0; Arrays.sort(list);
        byte[] buf = new byte[1 << 16];
        for (File f : list) {
            if (f.isDirectory()) { n += zipTree(out, f, prefix + f.getName() + "/"); continue; }
            ZipEntry e = new ZipEntry(prefix + f.getName()); e.setTime(f.lastModified()); out.putNextEntry(e);
            try (InputStream in = new FileInputStream(f)) { for (int k; (k = in.read(buf)) > 0;) out.write(buf, 0, k); }
            out.closeEntry(); n++;
        }
        return n;
    }
    /** The zip's entries' names, for SaveZip.scan: where the save is in it */
    private SaveZip.Layout scan(ContentResolver cr, Uri zip) throws IOException {
        List<String> names = new ArrayList<>();
        try (InputStream raw = cr.openInputStream(zip); ZipInputStream in = new ZipInputStream(raw)) {
            if (raw == null) throw new IOException("the file could not be opened");
            for (ZipEntry e; (e = in.getNextEntry()) != null;) names.add(SaveZip.norm(e.getName()) + (e.isDirectory() && !e.getName().endsWith("/") ? "/" : ""));
        }
        if (names.isEmpty()) throw new IOException("it is not a zip file, or it is empty");
        return SaveZip.scan(names, title);
    }
    private void importFrom(Uri zip) {
        ContentResolver cr = activity.getContentResolver(); String shown = name(cr, zip); SaveZip.Layout l;
        try { l = scan(cr, zip); } catch (IOException | RuntimeException e) { fail(IMPORT, shown + " could not be read: " + e.getMessage(), e); return; }
        if (l.unsafe != null) { fail(IMPORT, shown + " has a path that leaves its folder (" + l.unsafe + "): it is not imported.", null); return; }
        if (l.prefix == null) {
            fail(IMPORT, "This zip file does not match this game's title ID.\n\n" + appName() + " is title ID " + title + ": its saves are in a folder of that name in the zip."
                + (l.others.isEmpty() ? "" : "\n\n" + shown + " holds a save of title ID " + String.join(", ", l.others) + ", another game."), null);
            return;
        }
        if (l.files == 0) { fail(IMPORT, "The " + title + " folder in " + shown + " is empty.", null); return; }
        final int files = l.files; final String prefix = l.prefix;
        activity.runOnUiThread(() -> new AlertDialog.Builder(activity).setTitle("Import this save?")
            .setMessage(shown + "\n" + files + (files == 1 ? " file" : " files") + "\n\nYour current save is backed up first (in the app's saves folder), then the game closes: "
                + "open it again to play the imported save.")
            .setPositiveButton("Import and close", (d, b) -> work(() -> unpack(zip, prefix)))
            .setNegativeButton("Cancel", (d, b) -> finish(IMPORT, CANCELLED, ""))
            .setOnCancelListener(d -> finish(IMPORT, CANCELLED, "")).show());
    }
    private void unpack(Uri zip, String prefix) {   // into saves/<stamp>-zip (and .device), through hidden .part folders renamed once whole
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date()), name = stamp + "-zip";
        for (int k = 2; new File(dir, name).exists() && k < 100; k++) name = stamp + "-zip-" + k;
        File part = new File(dir, "." + name + ".part"), partDev = new File(dir, "." + name + ".part.device");
        String devPrefix = SaveZip.devicePrefix(prefix);
        ContentResolver cr = activity.getContentResolver(); byte[] buf = new byte[1 << 16];
        try {
            delete(part); delete(partDev);
            if (!part.mkdirs()) throw new IOException("cannot make " + part);
            try (InputStream raw = cr.openInputStream(zip); ZipInputStream in = new ZipInputStream(raw)) {
                if (raw == null) throw new IOException("the file could not be opened");
                for (ZipEntry e; (e = in.getNextEntry()) != null;) {
                    String entry = SaveZip.norm(e.getName()), n = SaveZip.inside(entry, prefix); File base = part;
                    if (n == null && (n = SaveZip.inside(entry, devPrefix)) != null) base = partDev;
                    if (n == null) continue;
                    File f = new File(base, n);
                    if (e.isDirectory() || n.endsWith("/")) { f.mkdirs(); continue; }
                    File up = f.getParentFile(); if (up != null && !up.isDirectory() && !up.mkdirs()) throw new IOException("cannot make " + up);
                    try (FileOutputStream out = new FileOutputStream(f)) { for (int k; (k = in.read(buf)) > 0;) out.write(buf, 0, k); out.getFD().sync(); }
                }
            }
            if (partDev.isDirectory() && !partDev.renameTo(new File(dir, name + ".device"))) throw new IOException("cannot name the device save");
            if (!part.renameTo(new File(dir, name))) throw new IOException("cannot name the save folder");   // the save's own folder last: once there, it is whole
        } catch (IOException | RuntimeException e) {
            delete(part); delete(partDev); delete(new File(dir, name + ".device"));
            fail(IMPORT, "The save could not be unpacked: " + e.getMessage(), e); return;
        }
        finish(IMPORT, OK, name);
    }

    // ---- the documents in a tree ----
    private static final class Doc { String id; boolean dir; long size; }
    private static Map<String, Doc> children(ContentResolver cr, Uri tree, String id) throws IOException {
        Map<String, Doc> out = new HashMap<>();
        try (Cursor c = cr.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, id), new String[] {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
                Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE}, null, null, null)) {
            if (c == null) throw new IOException("the folder could not be listed");
            while (c.moveToNext()) { Doc d = new Doc(); d.id = c.getString(0); d.dir = Document.MIME_TYPE_DIR.equals(c.getString(2)); d.size = c.isNull(3) ? -1 : c.getLong(3); out.put(c.getString(1), d); }
        }
        return out;
    }
    private static Doc find(Map<String, Doc> in, String name) {   // the exact name, else the same name in another case
        Doc d = in.get(name); if (d != null) return d;
        for (Map.Entry<String, Doc> e : in.entrySet()) if (e.getKey().equalsIgnoreCase(name)) return e.getValue();
        return null;
    }
    private static String mkdir(ContentResolver cr, Uri tree, String parent, String name) throws IOException {
        Uri u = DocumentsContract.createDocument(cr, DocumentsContract.buildDocumentUriUsingTree(tree, parent), Document.MIME_TYPE_DIR, name);
        if (u == null) throw new IOException("cannot make the folder " + name);
        return DocumentsContract.getDocumentId(u);
    }
    private static void download(ContentResolver cr, Uri tree, String id, File to) throws IOException {
        if (!to.isDirectory() && !to.mkdirs()) throw new IOException("cannot make " + to);
        byte[] buf = new byte[1 << 16];
        for (Map.Entry<String, Doc> e : children(cr, tree, id).entrySet()) {
            File f = new File(to, e.getKey());
            if (e.getValue().dir) { download(cr, tree, e.getValue().id, f); continue; }
            try (InputStream in = cr.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, e.getValue().id)); FileOutputStream out = new FileOutputStream(f)) {
                if (in == null) throw new IOException("cannot read " + e.getKey());
                for (int k; (k = in.read(buf)) > 0;) out.write(buf, 0, k);
                out.getFD().sync();
            }
        }
    }
    // local's tree into the folder id: a file that differs is written in place (the same file, its content replaced: a sync app sees one change, never the
    // file gone), a missing one is made, one the save no longer has is deleted; a file whose bytes are the same is left alone
    private static void upload(ContentResolver cr, Uri tree, File local, String id) throws IOException {
        Map<String, Doc> have = children(cr, tree, id);
        File[] list = local.listFiles(); if (list == null) list = new File[0];
        for (File f : list) {
            Doc d = have.remove(f.getName());
            if (d != null && d.dir != f.isDirectory()) { DocumentsContract.deleteDocument(cr, DocumentsContract.buildDocumentUriUsingTree(tree, d.id)); d = null; }
            if (f.isDirectory()) { upload(cr, tree, f, d != null ? d.id : mkdir(cr, tree, id, f.getName())); continue; }
            Uri u;
            if (d == null) {
                u = DocumentsContract.createDocument(cr, DocumentsContract.buildDocumentUriUsingTree(tree, id), "application/octet-stream", f.getName());
                if (u == null) throw new IOException("cannot make " + f.getName());
            } else {
                u = DocumentsContract.buildDocumentUriUsingTree(tree, d.id);
                if (d.size == f.length() && same(cr, u, f)) continue;
            }
            try (OutputStream out = cr.openOutputStream(u, "wt"); InputStream in = new FileInputStream(f)) {
                if (out == null) throw new IOException("cannot write " + f.getName());
                byte[] buf = new byte[1 << 16]; for (int k; (k = in.read(buf)) > 0;) out.write(buf, 0, k);
            }
        }
        for (Doc d : have.values()) DocumentsContract.deleteDocument(cr, DocumentsContract.buildDocumentUriUsingTree(tree, d.id));
    }
    private static boolean same(ContentResolver cr, Uri u, File f) {
        try (InputStream a = cr.openInputStream(u); InputStream b = new FileInputStream(f)) {
            if (a == null) return false;
            byte[] x = new byte[1 << 16], y = new byte[1 << 16];
            for (;;) {
                int n = readFull(a, x), m = readFull(b, y);
                if (n != m || !Arrays.equals(Arrays.copyOf(x, n), Arrays.copyOf(y, m))) return false;
                if (n < x.length) return true;
            }
        } catch (IOException | RuntimeException e) { return false; }
    }
    private static int readFull(InputStream in, byte[] b) throws IOException { int n = 0; for (int k; n < b.length && (k = in.read(b, n, b.length - n)) > 0;) n += k; return n; }

    // ---- helpers ----
    private synchronized boolean start(int kind, String titleId, String folder) {
        if (pending != 0) return false;
        pending = kind; title = titleId; dir = folder; return true;
    }
    private void finish(int kind, int code, String message) {
        synchronized (this) { if (pending != kind) return; pending = 0; }
        done(kind, code, message.getBytes(StandardCharsets.UTF_8));
    }
    private void fail(int kind, String message, Throwable why) {   // the player is told in a dialog, the log gets it too
        android.util.Log.e("dmhost", "[saves] " + message.replace('\n', ' '), why);
        activity.runOnUiThread(() -> {
            try {
                new AlertDialog.Builder(activity).setTitle(kind == IMPORT ? "Import save" : kind == EXPORT ? "Export save" : "Save file directory").setMessage(message)
                    .setPositiveButton(android.R.string.ok, null).setOnDismissListener(d -> finish(kind, FAILED, message)).show();
            } catch (RuntimeException e) { finish(kind, FAILED, message); }
        });
    }
    private void launch(Intent i, int kind) {
        try { activity.startActivityForResult(i, REQUEST + kind); }
        catch (ActivityNotFoundException e) { fail(kind, "This device has no file picker for this.", e); }
    }
    private static void work(Runnable r) { new Thread(r, "nx2a-saves").start(); }
    private String appName() {   // the app's name as a file name: the characters file systems refuse dropped
        String n = String.valueOf(activity.getApplicationInfo().loadLabel(activity.getPackageManager()));
        return n.replace(":", "").replaceAll("[\\\\/*?\"<>|]", "_").trim();
    }
    private static String name(ContentResolver cr, Uri u) {
        try (Cursor c = cr.query(u, new String[] {Document.COLUMN_DISPLAY_NAME}, null, null, null)) { if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0); }
        catch (RuntimeException ignored) {}
        String s = u.getLastPathSegment(); return s == null ? "the file" : s.substring(s.lastIndexOf('/') + 1);
    }
    private static void delete(File f) { File[] l = f.listFiles(); if (l != null) for (File c : l) delete(c); f.delete(); }
}
