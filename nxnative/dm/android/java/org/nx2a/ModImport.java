package org.nx2a;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.database.Cursor;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.ref.WeakReference;
import java.util.HashMap;
import java.util.HashSet;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class ModImport {
    private static final int REQUEST = 0x4d49;
    private static final long FILE_LIMIT = 512L * 1024 * 1024, TOTAL_LIMIT = 2L * 1024 * 1024 * 1024;
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static WeakReference<Activity> activity = new WeakReference<>(null);
    private static boolean busy;
    private static String notice;
    private static boolean imported;

    static void attach(Activity a) {
        activity = new WeakReference<>(a);
        if (notice != null) {
            String message = notice; notice = null;
            AlertDialog.Builder dialog = new AlertDialog.Builder(a).setTitle("Mod import").setMessage(message).setPositiveButton("OK", null);
            if (imported) dialog.setNeutralButton("Manage mods", (d, w) -> ModManager.open(a));
            imported = false; dialog.show();
        }
    }
    static void detach(Activity a) { if (activity.get() == a) activity.clear(); }
    static void open(Activity a) {
        a.runOnUiThread(() -> {
            if (a.isFinishing() || a.isDestroyed()) return;
            if (busy) {
                new AlertDialog.Builder(a).setMessage("A mod import is already in progress.").setPositiveButton("OK", null).show();
                return;
            }
            busy = true;
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
            intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/zip", "application/octet-stream"});
            try { a.startActivityForResult(intent, REQUEST); }
            catch (RuntimeException e) { busy = false; show("Cannot open the file picker: " + e.getMessage()); }
        });
    }
    static void result(Activity a, int request, int result, Intent data) {
        if (request != REQUEST) return;
        if (result != Activity.RESULT_OK || data == null || data.getData() == null) { busy = false; return; }
        Uri uri = data.getData();
        Context context = a.getApplicationContext();
        android.widget.Toast.makeText(a, "Importing mod ZIP...", android.widget.Toast.LENGTH_LONG).show();
        new Thread(() -> {
            String message;
            boolean success = false;
            try { message = "Imported to:\n" + extract(context, uri) + "\n\nImported files are not activated."; success = true; }
            catch (Exception e) { message = "Import failed: " + e.getMessage(); }
            String outcome = message; boolean completed = success;
            MAIN.post(() -> { busy = false; imported = completed; show(outcome); });
        }, "mod-import").start();
    }
    private static void show(String message) {
        notice = message;
        Activity a = activity.get();
        if (a != null) attach(a);
    }
    private static String extract(Context context, Uri uri) throws IOException {
        File root = new File(context.getExternalFilesDir(null).getCanonicalFile(), "usermods");
        if (!root.getCanonicalPath().equals(root.getAbsolutePath())) throw new IOException("Unsafe usermods directory");
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Cannot create usermods directory");
        String id = UUID.randomUUID().toString();
        String displayName = "Mod " + id.substring(0, 8);
        try (Cursor cursor = context.getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) displayName = cursor.getString(0);
        }
        File stage = new File(root, "." + id + ".part"), destination = new File(root, id);
        if (!stage.mkdir()) throw new IOException("Cannot create staging directory");
        boolean published = false, renamed = false;
        try {
            String prefix = stage.getCanonicalPath() + File.separator;
            HashMap<String, Boolean> paths = new HashMap<>();
            HashSet<String> entries = new HashSet<>();
            long total = 0;
            int count = 0;
            try (InputStream source = context.getContentResolver().openInputStream(uri);
                 ZipInputStream zip = new ZipInputStream(new FilterInputStream(source) {
                     long read;
                     @Override public int read() throws IOException {
                         int n = in.read(); if (n >= 0 && ++read > TOTAL_LIMIT) throw new IOException("ZIP exceeds 2 GiB"); return n;
                     }
                     @Override public int read(byte[] b, int off, int len) throws IOException {
                         int n = in.read(b, off, len); if (n > 0 && (read += n) > TOTAL_LIMIT) throw new IOException("ZIP exceeds 2 GiB"); return n;
                     }
                 })) {
                ZipEntry entry;
                byte[] buffer = new byte[65536];
                while ((entry = zip.getNextEntry()) != null) {
                    if (++count > 100000) throw new IOException("ZIP exceeds 100,000 entries");
                    String name = entry.getName();
                    if (name.startsWith("/") || name.indexOf('\\') >= 0 || name.indexOf('\0') >= 0)
                        throw new IOException("Invalid ZIP path");
                    boolean directory = entry.isDirectory();
                    if (directory) name = name.substring(0, name.length() - 1);
                    String[] parts = name.split("/", -1);
                    if (parts.length > 64) throw new IOException("ZIP path exceeds 64 components");
                    String path = "";
                    for (int i = 0; i < parts.length; i++) {
                        String part = parts[i];
                        if (part.isEmpty() || part.equals(".") || part.equals("..")) throw new IOException("Invalid ZIP path");
                        path = path.isEmpty() ? part : path + "/" + part;
                        boolean dir = i < parts.length - 1 || directory;
                        Boolean previous = paths.putIfAbsent(path, dir);
                        if (previous != null && previous.booleanValue() != dir) throw new IOException("Conflicting ZIP paths");
                    }
                    if (!entries.add(name)) throw new IOException("Duplicate ZIP entry");
                    File target = new File(stage, name);
                    if (!target.getCanonicalPath().startsWith(prefix)) throw new IOException("ZIP path escapes destination");
                    if (directory) {
                        if (zip.read() != -1) throw new IOException("Directory entry contains data");
                        if (!target.isDirectory() && !target.mkdirs()) throw new IOException("Cannot create directory");
                    } else {
                        File parent = target.getParentFile();
                        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create directory");
                        if (!target.createNewFile()) throw new IOException("Duplicate ZIP file");
                        long size = 0;
                        try (FileOutputStream out = new FileOutputStream(target)) {
                            int n;
                            while ((n = zip.read(buffer)) != -1) {
                                size += n; total += n;
                                if (size > FILE_LIMIT || total > TOTAL_LIMIT) throw new IOException("ZIP exceeds 512 MiB per file or 2 GiB total");
                                out.write(buffer, 0, n);
                            }
                        }
                    }
                    zip.closeEntry();
                }
            }
            if (count == 0) throw new IOException("ZIP contains no entries");
            String relative = ModManager.validate(stage);
            if (destination.exists() || !stage.renameTo(destination)) throw new IOException("Cannot publish import");
            renamed = true;
            ModManager.name(context, id, displayName, relative);
            published = true;
            return destination.getAbsolutePath();
        } finally {
            if (!published) { remove(stage); if (renamed) remove(destination); }
        }
    }
    private static void remove(File file) {
        if (file.isDirectory()) for (File child : file.listFiles()) remove(child);
        file.delete();
    }
}
