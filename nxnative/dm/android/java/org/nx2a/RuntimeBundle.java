package org.nx2a;

import android.content.Context;

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

    static byte[] readSmall(InputStream in) throws IOException {
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

    static void validateData(Context context, byte[] raw, byte[] source) throws Exception {
        JSONObject manifest = new JSONObject(new String(raw, java.nio.charset.StandardCharsets.UTF_8));
        JSONObject data = new JSONObject(new String(source, java.nio.charset.StandardCharsets.UTF_8));
        if (manifest.getInt("format") != 1 || !manifest.getString("package").equals(context.getPackageName()))
            throw new IOException("Runtime manifest format or package mismatch");
        String profile = manifest.getString("profile_h");
        if (!manifest.getString("title_id").equals(data.optString("title_id"))
                || !manifest.getString("version").equals(data.optString("version"))
                || ((data.has("runtime_bundle") || !profile.isEmpty()) && !profile.equals(data.optString("profile_h")))
                || (data.has("package") && !context.getPackageName().equals(data.optString("package"))))
            throw new IOException("External game data identity does not match the bundled runtime");
        if (data.has("runtime_bundle") && !hex(MessageDigest.getInstance("SHA-256").digest(raw)).equals(data.optString("runtime_bundle")))
            throw new IOException("External game data runtime bundle does not match this APK");
    }

    static void validateData(Context context, byte[] source) throws Exception {
        try (InputStream in = context.getAssets().open("runtime/manifest.json")) {
            validateData(context, readSmall(in), source);
        }
    }

    public static boolean checkData(android.app.Activity activity, String romfs) {
        File external = activity.getExternalFilesDir(null);
        String path = external == null ? "external files directory" : external.getAbsolutePath();
        try {
            if (external == null) throw new IOException("External game data is unavailable");
            byte[] raw;
            try (InputStream in = Files.newInputStream(external.toPath().resolve("nx2a-data.json"), LinkOption.NOFOLLOW_LINKS)) {
                raw = readSmall(in);
            }
            validateData(activity, raw);
            JSONObject data = new JSONObject(new String(raw, java.nio.charset.StandardCharsets.UTF_8));
            Path own = external.toPath().resolve("RomFS");
            if (new File(romfs).getCanonicalFile().equals(own.toFile().getCanonicalFile())) {
                try {
                    Files.readAttributes(own, BasicFileAttributes.class);
                    try (java.nio.file.DirectoryStream<Path> ignored = Files.newDirectoryStream(own)) {}
                } catch (java.nio.file.NoSuchFileException e) {
                    if (data.has("parts") && data.getJSONObject("parts").has("RomFS")) throw e;
                }
            }
            return true;
        } catch (Exception e) {
            String reason = e instanceof java.nio.file.NoSuchFileException ? "Game data is missing"
                    : e instanceof java.nio.file.AccessDeniedException || e instanceof SecurityException ? "Game data is not accessible"
                    : "Game data is invalid or unreadable";
            android.util.Log.e("nx2a", "[RED] " + reason + ": " + path, e);
            activity.startActivity(new android.content.Intent(activity, DataImportActivity.class)
                    .putExtra("error", reason + ": " + e.getMessage() + "\nExpected location: " + path));
            return false;
        }
    }

    private static boolean matches(InputStream in, Entry entry, FileOutputStream out) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        byte[] buffer = new byte[1024 * 1024];
        long size = 0;
        int n;
        while ((n = in.read(buffer)) != -1) {
            size += n;
            if (size > entry.size) return false;
            digest.update(buffer, 0, n);
            if (out != null) out.write(buffer, 0, n);
        }
        return size == entry.size && hex(digest.digest()).equals(entry.hash);
    }

    static void removeStage(Path path) throws IOException {
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
            Path path = root.resolve(entry.path);
            if (!Files.isDirectory(path.getParent(), LinkOption.NOFOLLOW_LINKS)
                    || !Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS))
                throw new IOException("Invalid private runtime cache: " + entry.path);
            try (InputStream in = Files.newInputStream(path, LinkOption.NOFOLLOW_LINKS)) {
                if (!matches(in, entry, null)) throw new IOException("Invalid private runtime cache: " + entry.path);
            }
        }
    }

    private static void cleanup(Path root, List<Entry> entries) {
        for (Entry entry : entries) {
            Path path = root.resolve(entry.path);
            try {
                if (Files.isSymbolicLink(root) || (entry.path.startsWith("mods/")
                        && !Files.isDirectory(root.resolve("mods"), LinkOption.NOFOLLOW_LINKS))
                        || !Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) {
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
                }
            } catch (Exception ignored) {
            }
        }
        try {
            Path mods = root.resolve("mods");
            if (Files.isDirectory(mods, LinkOption.NOFOLLOW_LINKS)) Files.delete(mods);
        } catch (DirectoryNotEmptyException ignored) {
        } catch (IOException ignored) {
        }
    }

    public static String prepare(Context context) throws Exception {
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
        String id = hex(MessageDigest.getInstance("SHA-256").digest(raw));
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

        File external = context.getExternalFilesDir(null);
        if (external == null) throw new IOException("External game data is unavailable");
        try (InputStream in = Files.newInputStream(external.toPath().resolve("nx2a-data.json"), LinkOption.NOFOLLOW_LINKS)) {
            validateData(context, raw, readSmall(in));
        }

        Path parent = context.getFilesDir().toPath().resolve("runtime");
        if (Files.isSymbolicLink(parent)) throw new IOException("Invalid private runtime cache directory");
        Files.createDirectories(parent);
        if (!Files.isDirectory(parent, LinkOption.NOFOLLOW_LINKS))
            throw new IOException("Invalid private runtime cache directory");
        Path root = parent.resolve(id);
        if (Files.exists(root, LinkOption.NOFOLLOW_LINKS)) {
            verifyRoot(root, raw, files);
        } else {
            Path stage = Files.createTempDirectory(parent, id + ".partial-");
            try {
                for (Entry entry : files) {
                    Path path = stage.resolve(entry.path);
                    Files.createDirectories(path.getParent());
                    try (InputStream in = context.getAssets().open("runtime/" + entry.path);
                         FileOutputStream out = new FileOutputStream(path.toFile())) {
                        if (!matches(in, entry, out)) throw new IOException("Runtime asset mismatch: " + entry.path);
                        out.getFD().sync();
                    }
                }
                try (FileOutputStream out = new FileOutputStream(stage.resolve("manifest.json").toFile())) {
                    out.write(raw);
                    out.getFD().sync();
                }
                android.system.Os.rename(stage.toString(), root.toString());
            } finally {
                if (Files.exists(stage, LinkOption.NOFOLLOW_LINKS)) removeStage(stage);
            }
        }
        cleanup(external.toPath(), cleanup);
        return root.toAbsolutePath().toString();
    }
}
