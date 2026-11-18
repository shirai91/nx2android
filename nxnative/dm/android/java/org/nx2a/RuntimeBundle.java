package org.nx2a;

import android.content.Context;
import android.os.Build;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.channels.Channels;
import java.nio.file.DirectoryNotEmptyException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public final class RuntimeBundle {
    private static final int LIMIT = 4 * 1024 * 1024;
    private static native void startupTrace(String message);

    private RuntimeBundle() {}

    private static final class Entry {
        final String path, hash;
        final long size;

        Entry(JSONObject json, boolean cleanup) throws Exception {
            path = json.getString("path");
            hash = json.getString("sha256");
            size = json.getLong("size");
            String suffix = cleanup ? "(sites|cuts|obs)\\.tsv" : "(patched\\.bin|fixups\\.tsv|gates\\.tsv)";
            boolean module = path.matches("mods/[A-Za-z0-9_-]+\\." + suffix);
            boolean payload = path.equals("mods/nro.tsv") || path.equals("fpslock.tsv")
                    || path.equals("shaders.dmsv") || path.equals("nintendo_ext.ttf");
            if ((!module && !(cleanup ? path.equals("mods/build_src.sha256") : payload))
                    || size < 0 || !hash.matches("[0-9a-f]{64}"))
                throw new IOException("Invalid runtime manifest entry: " + path);
        }
    }

    private static byte[] readSmall(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int n;
        while ((n = in.read(buffer)) != -1) {
            if (out.size() + n > LIMIT) throw new IOException("Runtime manifest too large");
            out.write(buffer, 0, n);
        }
        return out.toByteArray();
    }

    private static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) out.append(String.format("%02x", b & 255));
        return out.toString();
    }

    private static boolean matches(InputStream in, Entry entry, FileOutputStream out) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        byte[] buffer = new byte[1024 * 1024];
        long size = 0;
        int n;
        while ((n = in.read(buffer)) != -1) {
            size += n;
            if (size > entry.size) { startupTrace("[runtime] mismatch " + entry.path + " size exceeds expected " + entry.size + " (read " + size + ")"); return false; }
            digest.update(buffer, 0, n);
            if (out != null) out.write(buffer, 0, n);
        }
        String actual = hex(digest.digest());
        boolean match = size == entry.size && actual.equals(entry.hash);
        if (!match) startupTrace("[runtime] mismatch " + entry.path + " size " + size + "/" + entry.size + " sha256 " + actual + "/" + entry.hash);
        return match;
    }

    private static void removeStage(Path path) throws IOException {
        if (Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS)) {
            try (java.nio.file.DirectoryStream<Path> children = Files.newDirectoryStream(path)) {
                for (Path child : children) removeStage(child);
            }
        }
        Files.delete(path);
    }

    private static void verifyRoot(Path root, byte[] manifest, List<Entry> files) throws Exception {
        if (!Files.isDirectory(root, LinkOption.NOFOLLOW_LINKS))
            throw new IOException("Invalid private runtime cache; clear the app's runtime cache");
        Path marker = root.resolve("manifest.json");
        try (InputStream in = Files.newInputStream(marker, LinkOption.NOFOLLOW_LINKS)) {
            if (!MessageDigest.isEqual(manifest, readSmall(in)))
                throw new IOException("Invalid private runtime cache manifest");
        }
        for (Entry entry : files) {
            long start = System.nanoTime();
            startupTrace("[runtime] verify BEGIN " + entry.path + " size=" + entry.size);
            Path path = root.resolve(entry.path);
            if (!Files.isDirectory(path.getParent(), LinkOption.NOFOLLOW_LINKS)
                    || !Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS))
                throw new IOException("Invalid private runtime cache: " + entry.path);
            try (InputStream in = Files.newInputStream(path, LinkOption.NOFOLLOW_LINKS)) {
                if (!matches(in, entry, null)) throw new IOException("Invalid private runtime cache: " + entry.path);
            }
            startupTrace("[runtime] verify DONE " + entry.path + " hash matched ms=" + (System.nanoTime() - start) / 1000000);
        }
    }

    private static void cleanup(Path root, List<Entry> entries) {
        long start = System.nanoTime(), bytes = 0;
        int deleted = 0, retained = 0;
        startupTrace("[runtime] cleanup BEGIN root=" + root + " entries=" + entries.size());
        for (Entry entry : entries) {
            Path path = root.resolve(entry.path);
            try {
                if (Files.isSymbolicLink(root) || (entry.path.startsWith("mods/")
                        && !Files.isDirectory(root.resolve("mods"), LinkOption.NOFOLLOW_LINKS))
                        || !Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) {
                    boolean exists = Files.exists(path, LinkOption.NOFOLLOW_LINKS);
                    startupTrace("[runtime] cleanup retained " + entry.path + (exists ? " not regular or unsafe parent" : " missing or unsafe parent"));
                    retained++;
                    continue;
                }
                BasicFileAttributes before = Files.readAttributes(path, BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
                boolean match;
                try (InputStream in = Channels.newInputStream(Files.newByteChannel(path,
                        StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS))) {
                    match = matches(in, entry, null);
                }
                BasicFileAttributes after = Files.readAttributes(path, BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
                if (match && before.fileKey() != null && before.fileKey().equals(after.fileKey())
                        && before.size() == after.size() && before.lastModifiedTime().equals(after.lastModifiedTime())
                        && !Files.isSymbolicLink(root.resolve("mods"))) {
                    Files.delete(path);
                    deleted++; bytes += before.size();
                    startupTrace("[runtime] cleanup deleted " + entry.path + " bytes=" + before.size());
                } else { retained++; startupTrace("[runtime] cleanup retained " + entry.path + (!match ? " content differs" : before.fileKey() == null ? " identity unavailable" : " identity changed or unsafe parent")); }
            } catch (Exception e) {
                retained++; startupTrace("[runtime] cleanup retained " + entry.path + " error=" + e);
            }
        }
        try {
            Path mods = root.resolve("mods");
            if (Files.isDirectory(mods, LinkOption.NOFOLLOW_LINKS)) Files.delete(mods);
        } catch (DirectoryNotEmptyException ignored) {
        } catch (IOException e) {
            startupTrace("[runtime] retained mods directory: " + e);
        }
        startupTrace("[runtime] cleanup DONE deleted=" + deleted + " retained=" + retained + " logical_bytes=" + bytes + " ms=" + (System.nanoTime() - start) / 1000000);
    }

    public static String prepare(Context context) throws Exception {
        long started = System.nanoTime();
        startupTrace("[runtime] prepare BEGIN package=" + context.getPackageName() + " manufacturer=" + Build.MANUFACTURER
                + " brand=" + Build.BRAND + " model=" + Build.MODEL + " device=" + Build.DEVICE
                + " sdk=" + Build.VERSION.SDK_INT + " release=" + Build.VERSION.RELEASE
                + " incremental=" + Build.VERSION.INCREMENTAL + " display=" + Build.DISPLAY);
        android.content.pm.PackageInfo app = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
        startupTrace("[runtime] app version=" + app.versionName + " code=" + app.getLongVersionCode());
        startupTrace("[runtime] opening APK manifest");
        InputStream manifestStream;
        try {
            manifestStream = context.getAssets().open("runtime/manifest.json");
        } catch (FileNotFoundException e) {
            return null;
        }
        byte[] raw;
        try (InputStream in = manifestStream) { raw = readSmall(in); }
        JSONObject manifest = new JSONObject(new String(raw, java.nio.charset.StandardCharsets.UTF_8));
        if (manifest.getInt("format") != 1 || !manifest.getString("package").equals(context.getPackageName()))
            throw new IOException("Runtime manifest format or package mismatch");
        String title = manifest.getString("title_id"), version = manifest.getString("version");
        String profile = manifest.getString("profile_h");
        String id = hex(MessageDigest.getInstance("SHA-256").digest(raw));
        startupTrace("[runtime] manifest id=" + id + " format=" + manifest.getInt("format") + " package=" + manifest.getString("package")
                + " title=" + title + " version=" + version + " profile_h=" + profile);
        List<Entry> files = new ArrayList<>(), cleanup = new ArrayList<>();
        Set<String> paths = new HashSet<>();
        JSONArray payload = manifest.getJSONArray("files"), reports = manifest.getJSONArray("cleanup");
        for (int i = 0; i < payload.length(); i++) {
            Entry entry = new Entry(payload.getJSONObject(i), false);
            if (!paths.add(entry.path)) throw new IOException("Duplicate runtime entry: " + entry.path);
            files.add(entry); cleanup.add(entry);
        }
        if (!paths.contains("mods/main.patched.bin") || !paths.contains("mods/main.fixups.tsv"))
            throw new IOException("Runtime manifest is missing the main module");
        for (int i = 0; i < reports.length(); i++) {
            Entry entry = new Entry(reports.getJSONObject(i), true);
            if (!paths.add(entry.path)) throw new IOException("Duplicate runtime entry: " + entry.path);
            cleanup.add(entry);
        }
        long total = 0;
        for (Entry entry : files) total += entry.size;
        startupTrace("[runtime] manifest files=" + files.size() + " bytes=" + total + " cleanup_reports=" + reports.length());

        File external = context.getExternalFilesDir(null);
        if (external == null) throw new IOException("External game data is unavailable");
        startupTrace("[runtime] reading external identity root=" + external);
        JSONObject data;
        try (InputStream in = Files.newInputStream(external.toPath().resolve("nx2a-data.json"), LinkOption.NOFOLLOW_LINKS)) {
            data = new JSONObject(new String(readSmall(in), java.nio.charset.StandardCharsets.UTF_8));
        }
        startupTrace("[runtime] external identity package=" + data.optString("package") + " title=" + data.optString("title_id")
                + " version=" + data.optString("version") + " profile_h=" + data.optString("profile_h") + " runtime_bundle=" + data.optString("runtime_bundle"));
        if (!title.equals(data.optString("title_id")) || !version.equals(data.optString("version"))
                || ((data.has("runtime_bundle") || !profile.isEmpty()) && !profile.equals(data.optString("profile_h")))
                || (data.has("package") && !context.getPackageName().equals(data.optString("package"))))
            throw new IOException("External game data identity does not match the bundled runtime");
        if (data.has("runtime_bundle")) {
            if (!id.equals(data.optString("runtime_bundle")))
                throw new IOException("External game data runtime bundle does not match this APK");
        } else startupTrace("[runtime] legacy data identity matched; RomFS bytes not verified");

        Path parent = context.getFilesDir().toPath().resolve("runtime");
        startupTrace("[runtime] preparing private cache parent=" + parent);
        if (Files.isSymbolicLink(parent)) throw new IOException("Invalid private runtime cache directory");
        Files.createDirectories(parent);
        if (!Files.isDirectory(parent, LinkOption.NOFOLLOW_LINKS))
            throw new IOException("Invalid private runtime cache directory");
        Path root = parent.resolve(id);
        if (Files.exists(root, LinkOption.NOFOLLOW_LINKS)) {
            startupTrace("[runtime] reuse BEGIN root=" + root);
            verifyRoot(root, raw, files);
            startupTrace("[runtime] reuse DONE root=" + root);
        } else {
            Path stage = Files.createTempDirectory(parent, id + ".partial-");
            startupTrace("[runtime] staging BEGIN path=" + stage);
            try {
                for (Entry entry : files) {
                    long start = System.nanoTime();
                    startupTrace("[runtime] extract BEGIN " + entry.path + " size=" + entry.size);
                    Path path = stage.resolve(entry.path);
                    Files.createDirectories(path.getParent());
                    try (InputStream in = context.getAssets().open("runtime/" + entry.path);
                         FileOutputStream out = new FileOutputStream(path.toFile())) {
                        if (!matches(in, entry, out)) throw new IOException("Runtime asset mismatch: " + entry.path);
                        out.getFD().sync();
                    }
                    startupTrace("[runtime] extract DONE " + entry.path + " hash matched ms=" + (System.nanoTime() - start) / 1000000);
                }
                startupTrace("[runtime] writing staged manifest");
                try (FileOutputStream out = new FileOutputStream(stage.resolve("manifest.json").toFile())) {
                    out.write(raw);
                    out.getFD().sync();
                }
                startupTrace("[runtime] publish BEGIN " + stage + " -> " + root);
                android.system.Os.rename(stage.toString(), root.toString());
                startupTrace("[runtime] publish DONE root=" + root);
            } finally {
                if (Files.exists(stage, LinkOption.NOFOLLOW_LINKS)) removeStage(stage);
            }
        }
        cleanup(external.toPath(), cleanup);
        startupTrace("[runtime] prepare DONE root=" + root + " ms=" + (System.nanoTime() - started) / 1000000);
        return root.toAbsolutePath().toString();
    }
}
