package org.nx2a;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.DisplayCutout;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.BufferedInputStream;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.HashMap;
import java.util.Map;
import java.util.List;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class DataImportActivity extends Activity {
    private TextView message;
    private Button importButton, zipButton, closeButton;
    private ProgressBar progress;
    private boolean copying, imported;

    private static final class Document {
        String name, mime, id;
        Document(String name, String mime, String id) {
            this.name = name; this.mime = mime; this.id = id;
        }
    }

    @Override public void onCreate(Bundle state) {
        setTheme(android.R.style.Theme_Material_Light_NoActionBar);
        super.onCreate(state);
        int padding = (int) (16 * getResources().getDisplayMetrics().density + 0.5f);
        ScrollView scroll = new ScrollView(this);
        scroll.setOnApplyWindowInsetsListener((view, insets) -> {
            DisplayCutout cutout = insets.getDisplayCutout();
            int left = insets.getSystemWindowInsetLeft(), top = insets.getSystemWindowInsetTop();
            int right = insets.getSystemWindowInsetRight(), bottom = insets.getSystemWindowInsetBottom();
            if (cutout != null) {
                left = Math.max(left, cutout.getSafeInsetLeft());
                top = Math.max(top, cutout.getSafeInsetTop());
                right = Math.max(right, cutout.getSafeInsetRight());
                bottom = Math.max(bottom, cutout.getSafeInsetBottom());
            }
            view.setPadding(padding + left, padding + top, padding + right, padding + bottom);
            return insets;
        });
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(layout);
        message = new TextView(this);
        message.setText(getIntent().getStringExtra("error") + "\n\nSelect an extracted data folder outside Android/data or a data ZIP.\nImport replaces current game data. Saves and backups are kept. Keep this app open until import finishes.");
        layout.addView(message);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setIndeterminate(true);
        progress.setVisibility(View.GONE);
        layout.addView(progress);
        importButton = new Button(this);
        importButton.setText("Import folder");
        importButton.setOnClickListener(v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), 1));
        layout.addView(importButton);
        zipButton = new Button(this);
        zipButton.setText("Import ZIP");
        zipButton.setOnClickListener(v -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("application/zip");
            intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/zip", "application/x-zip-compressed", "application/octet-stream"});
            startActivityForResult(intent, 2);
        });
        layout.addView(zipButton);
        closeButton = new Button(this);
        closeButton.setText("Close");
        closeButton.setOnClickListener(v -> {
            if (copying) return;
            if (imported) {
                finishAndRemoveTask();
                android.os.Process.killProcess(android.os.Process.myPid());
            } else finishAffinity();
        });
        layout.addView(closeButton);
        setContentView(scroll);
        scroll.requestApplyInsets();
    }

    @Override public void onBackPressed() {
        if (!copying) finishAffinity();
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if ((request != 1 && request != 2) || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri tree = data.getData();
        copying = true;
        importButton.setEnabled(false);
        zipButton.setEnabled(false);
        progress.setVisibility(View.VISIBLE);
        message.setText(request == 1 ? "Checking data folder. Keep this app open until import finishes."
                : "Clearing old game data. Keep this app open until import finishes.");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        new Thread(() -> {
            try {
                if (request == 1) importData(tree); else importZip(tree);
                runOnUiThread(() -> {
                    copying = false;
                    imported = true;
                    progress.setVisibility(View.GONE);
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    message.setText("Import complete. Close the app, then reopen it to play.");
                    importButton.setVisibility(View.GONE);
                    zipButton.setVisibility(View.GONE);
                    closeButton.setText("Close app");
                });
            } catch (Exception e) {
                Log.e("nx2a", "[RED] Data import failed from " + tree, e);
                runOnUiThread(() -> {
                    copying = false;
                    progress.setVisibility(View.GONE);
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                    message.setText(request == 2
                            ? "Import failed: " + e.getMessage() + "\nZIP import did not complete. Retrying clears partial game data. Saves and backups are kept. Keep this app open until import finishes."
                            : "Import failed: " + e.getMessage() + "\nSelect the data folder or ZIP and try again.\nImport replaces current game data. Saves and backups are kept. Keep this app open until import finishes.");
                    importButton.setEnabled(true);
                    zipButton.setEnabled(true);
                });
            }
        }).start();
    }

    private void importing(String source) {
        runOnUiThread(() -> message.setText("Importing " + source + ". Keep this app open until import finishes."));
    }

    private List<Document> children(Uri tree, String id) throws IOException {
        List<Document> result = new ArrayList<>();
        Set<String> names = new HashSet<>();
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor cursor = getContentResolver().query(uri, columns, null, null, null)) {
            if (cursor == null) throw new IOException("Cannot list source folder: " + uri);
            while (cursor.moveToNext()) {
                String name = cursor.getString(1);
                if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") || name.contains("/") || name.contains("\\") || !names.add(name))
                    throw new IOException("Invalid or duplicate source name: " + name);
                result.add(new Document(name, cursor.getString(2), cursor.getString(0)));
            }
        }
        return result;
    }

    private void copy(Uri tree, Document doc, Path path, byte[] buffer) throws IOException {
        if (!Files.isDirectory(path.getParent(), LinkOption.NOFOLLOW_LINKS) || Files.isSymbolicLink(path))
            throw new IOException("Unsafe destination: " + path);
        Uri uri = DocumentsContract.buildDocumentUriUsingTree(tree, doc.id);
        if (DocumentsContract.Document.MIME_TYPE_DIR.equals(doc.mime)) {
            if (!Files.exists(path, LinkOption.NOFOLLOW_LINKS)) Files.createDirectory(path);
            if (!Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS)) throw new IOException("Not a directory: " + path);
            for (Document child : children(tree, doc.id)) copy(tree, child, path.resolve(child.name), buffer);
        } else {
            try (InputStream in = getContentResolver().openInputStream(uri);
                 OutputStream out = Files.newOutputStream(path, StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING, StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS)) {
                if (in == null) throw new IOException("Cannot open source: " + uri);
                int n;
                while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
            }
        }
    }

    private void importData(Uri tree) throws Exception {
        List<Document> roots = children(tree, DocumentsContract.getTreeDocumentId(tree));
        Set<String> names = new HashSet<>();
        Document manifest = null;
        for (Document doc : roots) {
            names.add(doc.name);
            if (doc.name.equals("nx2a-data.json")) manifest = doc;
            if ((doc.name.equals("RomFS") || doc.name.equals("aoc") || doc.name.equals("sysdata"))
                    && !DocumentsContract.Document.MIME_TYPE_DIR.equals(doc.mime))
                throw new IOException("Expected a directory: " + doc.name);
            if ((doc.name.equals("standard.ttf") || doc.name.equals("env.txt") || doc.name.equals("settings.txt"))
                    && DocumentsContract.Document.MIME_TYPE_DIR.equals(doc.mime))
                throw new IOException("Expected a file: " + doc.name);
        }
        if (manifest == null || DocumentsContract.Document.MIME_TYPE_DIR.equals(manifest.mime))
            throw new IOException("The selected folder must contain nx2a-data.json");
        byte[] raw;
        Uri uri = DocumentsContract.buildDocumentUriUsingTree(tree, manifest.id);
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("Cannot open source manifest: " + uri);
            raw = RuntimeBundle.readSmall(in);
        }
        validateRoots(raw, names);
        importing("data");
        Path root = resetData();
        byte[] buffer = new byte[1024 * 1024];
        for (Document doc : roots) {
            if (doc.name.equals("RomFS") || doc.name.equals("aoc") || doc.name.equals("sysdata")
                    || doc.name.equals("standard.ttf") || doc.name.equals("env.txt") || doc.name.equals("settings.txt"))
                copy(tree, doc, root.resolve(doc.name), buffer);
        }
        publishData(root, raw);
    }

    private void validateRoots(byte[] raw, Set<String> names) throws Exception {
        RuntimeBundle.validateData(this, raw);
        org.json.JSONObject data = new org.json.JSONObject(new String(raw, java.nio.charset.StandardCharsets.UTF_8));
        if (data.has("parts")) {
            org.json.JSONObject parts = data.getJSONObject("parts");
            if (parts.has("RomFS") && !names.contains("RomFS")) throw new IOException("Source is missing RomFS");
            if (parts.has("aoc") && !names.contains("aoc")) throw new IOException("Source is missing aoc");
        }
        if (data.has("dlc") && data.getJSONArray("dlc").length() > 0 && !names.contains("aoc"))
            throw new IOException("Source is missing aoc");
        if (data.has("sysdata") && data.getJSONArray("sysdata").length() > 0 && !names.contains("sysdata"))
            throw new IOException("Source is missing sysdata");
    }

    private Path resetData() throws IOException {
        Path root = getExternalFilesDir(null).toPath();
        if (!Files.isDirectory(root, LinkOption.NOFOLLOW_LINKS)) throw new IOException("Unsafe destination: " + root);
        Path marker = root.resolve("nx2a-data.json"), temp = root.resolve("nx2a-data.json.tmp");
        if (Files.isSymbolicLink(marker) || Files.isSymbolicLink(temp)) throw new IOException("Unsafe destination manifest");
        Files.deleteIfExists(marker);
        Files.deleteIfExists(temp);
        for (String name : new String[]{"RomFS", "aoc", "sysdata", "standard.ttf", "env.txt", "settings.txt"}) {
            Path path = root.resolve(name);
            if (Files.exists(path, LinkOption.NOFOLLOW_LINKS)) RuntimeBundle.removeStage(path);
        }
        return root;
    }

    private void publishData(Path root, byte[] raw) throws IOException {
        Path marker = root.resolve("nx2a-data.json"), temp = root.resolve("nx2a-data.json.tmp");
        Path romfs = root.resolve("RomFS");
        if (!Files.exists(romfs, LinkOption.NOFOLLOW_LINKS)) Files.createDirectory(romfs);
        if (!Files.isDirectory(romfs, LinkOption.NOFOLLOW_LINKS)) throw new IOException("Unsafe destination: " + romfs);
        try (OutputStream out = Files.newOutputStream(temp, StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING, StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS)) {
            out.write(raw);
        }
        Files.move(temp, marker, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
    }

    private String zipPath(ZipEntry entry, Map<String, Boolean> paths, Set<String> entries) throws IOException {
        String name = entry.getName();
        if (name.isEmpty() || name.startsWith("/") || name.matches("^[A-Za-z]:.*") || name.contains("\\") || name.indexOf('\0') >= 0)
            throw new IOException("Invalid ZIP path: " + name);
        String path = entry.isDirectory() ? name.substring(0, name.length() - 1) : name;
        String[] parts = path.split("/", -1);
        String parent = "";
        for (int i = 0; i < parts.length; i++) {
            String part = parts[i];
            if (part.isEmpty() || part.equals(".") || part.equals(".."))
                throw new IOException("Invalid ZIP path: " + name);
            parent = parent.isEmpty() ? part : parent + "/" + part;
            boolean directory = i < parts.length - 1 || entry.isDirectory();
            if (paths.containsKey(parent) && paths.get(parent) != directory)
                throw new IOException("ZIP file/directory collision: " + parent);
            paths.put(parent, directory);
        }
        if (!entries.add(path)) throw new IOException("Duplicate ZIP entry: " + path);
        return path;
    }

    private ZipInputStream openZip(Uri uri) throws IOException {
        InputStream in = getContentResolver().openInputStream(uri);
        if (in == null) throw new IOException("Cannot open ZIP: " + uri);
        return new ZipInputStream(new BufferedInputStream(in, 1024 * 1024));
    }

    private void importZip(Uri uri) throws Exception {
        Path root = resetData();
        importing("ZIP");
        Map<String, Boolean> paths = new HashMap<>();
        Set<String> entries = new HashSet<>();
        Set<String> roots = new HashSet<>();
        byte[] buffer = new byte[1024 * 1024], raw = null;
        String prefix = null;
        long unpacked = 0, lastUpdate = 0;
        try (ZipInputStream zip = openZip(uri)) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                String path = zipPath(entry, paths, entries);
                String[] components = path.split("/", 3);
                String top = components[0], candidate = "";
                boolean managed = top.equals("RomFS") || top.equals("aoc") || top.equals("sysdata")
                        || top.equals("standard.ttf") || top.equals("env.txt") || top.equals("settings.txt")
                        || top.equals("nx2a-data.json");
                if ((!managed && components.length > 1)
                        || (components.length == 2 && components[1].equals("nx2a-data.json"))) {
                    candidate = top + "/";
                    top = components[1];
                    managed = top.equals("RomFS") || top.equals("aoc") || top.equals("sysdata")
                            || top.equals("standard.ttf") || top.equals("env.txt") || top.equals("settings.txt")
                            || top.equals("nx2a-data.json");
                }
                String relative = path.substring(candidate.length());
                boolean directoryRoot = top.equals("RomFS") || top.equals("aoc") || top.equals("sysdata");
                boolean fileRoot = top.equals("standard.ttf") || top.equals("env.txt") || top.equals("settings.txt");
                boolean manifest = top.equals("nx2a-data.json") && relative.equals(top);
                if (managed && (directoryRoot || relative.equals(top))) {
                    if (prefix != null && !prefix.equals(candidate)) throw new IOException("Multiple ZIP data roots");
                    prefix = candidate;
                    if (manifest) {
                        if (raw != null || entry.isDirectory()) throw new IOException("Invalid ZIP data manifest");
                        raw = RuntimeBundle.readSmall(zip);
                        unpacked += raw.length;
                    } else {
                        if (relative.equals(top) && directoryRoot && !entry.isDirectory())
                            throw new IOException("Expected a directory: " + top);
                        if (fileRoot && entry.isDirectory()) throw new IOException("Expected a file: " + top);
                        roots.add(top);
                        Path dest = root;
                        String[] parts = relative.split("/");
                        for (int i = 0; i < parts.length; i++) {
                            dest = dest.resolve(parts[i]);
                            if (i < parts.length - 1 || entry.isDirectory()) {
                                if (!Files.exists(dest, LinkOption.NOFOLLOW_LINKS)) Files.createDirectory(dest);
                                if (!Files.isDirectory(dest, LinkOption.NOFOLLOW_LINKS)) throw new IOException("Unsafe destination: " + dest);
                            }
                        }
                        if (!entry.isDirectory()) {
                            try (OutputStream out = Files.newOutputStream(dest, StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING, StandardOpenOption.WRITE, LinkOption.NOFOLLOW_LINKS)) {
                                int n;
                                while ((n = zip.read(buffer)) != -1) {
                                    out.write(buffer, 0, n);
                                    unpacked += n;
                                    long now = SystemClock.uptimeMillis();
                                    if (now - lastUpdate >= 250) {
                                        importing("ZIP: " + (unpacked / (1024 * 1024)) + " MiB, " + relative);
                                        lastUpdate = now;
                                    }
                                }
                            }
                        }
                    }
                }
                int n;
                while ((n = zip.read(buffer)) != -1) {
                    unpacked += n;
                    long now = SystemClock.uptimeMillis();
                    if (now - lastUpdate >= 250) {
                        importing("ZIP: " + (unpacked / (1024 * 1024)) + " MiB, " + path);
                        lastUpdate = now;
                    }
                }
                zip.closeEntry();
                if (entry.isDirectory() && entry.getSize() != 0) throw new IOException("ZIP directory has data: " + path);
            }
        }
        if (raw == null) throw new IOException("ZIP must contain nx2a-data.json at its root or inside one folder");
        validateRoots(raw, roots);
        publishData(root, raw);
    }
}
