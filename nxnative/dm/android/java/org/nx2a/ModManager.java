package org.nx2a;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.FileVisitResult;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.UUID;

final class ModManager {
    private static String applied = "";
    private static boolean conflict;
    private static boolean deleting;

    private static SharedPreferences state(Context c) { return c.getSharedPreferences("mod-selection", Context.MODE_PRIVATE); }
    private static File root(Context c) { return new File(c.getExternalFilesDir(null), "usermods"); }

    private static void scan(File dir, int depth, ArrayList<File> found) throws IOException {
        if (!dir.getCanonicalPath().equals(dir.getAbsolutePath())) throw new IOException("Link in mod path");
        File[] children = dir.listFiles();
        if (children == null) throw new IOException("Cannot read mod directory");
        for (File child : children) {
            if (!child.getCanonicalPath().equals(child.getAbsolutePath()) || child.getName().indexOf(':') >= 0) throw new IOException("Unsafe path in mod");
            if (!child.isDirectory()) continue;
            if (child.getName().equals("romfs")) {
                found.add(child);
            } else if (depth < 3) scan(child, depth + 1, found);
        }
    }
    private static void links(File dir) throws IOException {
        if (!dir.getCanonicalPath().equals(dir.getAbsolutePath())) throw new IOException("Link in mod");
        if (dir.isDirectory()) {
            File[] children = dir.listFiles();
            if (children == null) throw new IOException("Cannot read mod directory");
            for (File child : children) links(child);
        }
    }
    static String validate(File dir) throws IOException {
        ArrayList<File> found = new ArrayList<>();
        scan(dir, 0, found);
        if (found.size() != 1) throw new IOException("Expected one romfs folder");
        links(dir);
        return dir.toPath().relativize(found.get(0).toPath()).toString();
    }
    private static File resolve(Context c, String id) throws IOException {
        try { if (!UUID.fromString(id).toString().equals(id)) throw new IllegalArgumentException(); }
        catch (IllegalArgumentException e) { throw new IOException("Invalid mod ID"); }
        File external = c.getExternalFilesDir(null).getCanonicalFile();
        File base = new File(external, "usermods"), dir = new File(base, id);
        if (!base.getCanonicalPath().equals(base.getAbsolutePath())) throw new IOException("Unsafe usermods directory");
        if (!dir.isDirectory() || !dir.getCanonicalPath().equals(dir.getAbsolutePath())) throw new IOException("Missing or unsafe mod");
        String relative = state(c).getString("path." + id, "");
        if (relative.isEmpty()) throw new IOException("Mod not validated; reimport ZIP");
        File romfs = new File(dir, relative);
        if (!romfs.getCanonicalPath().equals(romfs.getAbsolutePath()) || !romfs.getCanonicalPath().startsWith(dir.getCanonicalPath() + File.separator)
                || !romfs.getName().equals("romfs") || !romfs.isDirectory())
            throw new IOException("Missing or unsafe mod RomFS; reimport ZIP");
        return romfs;
    }
    static String start(Activity a, boolean hasOverlay) {
        conflict = hasOverlay; applied = "";
        String selected = state(a).getString("selected", "");
        if (selected.isEmpty()) { Log.i("dmhost", "[mod] no mod selected; original RomFS"); return null; }
        if (conflict) { Log.e("dmhost", "[RED] [mod] existing DM_ROMFS_OVERLAY preserved; selected mod not applied"); return null; }
        try {
            File path = resolve(a, selected); applied = selected;
            Log.i("dmhost", "[mod] selected " + selected + " overlay " + path);
            return path.getAbsolutePath();
        } catch (IOException e) {
            Log.e("dmhost", "[RED] [mod] selected mod not applied: " + e.getMessage());
            return null;
        }
    }
    static void name(Context c, String id, String name, String path) throws IOException {
        if (!state(c).edit().putString("name." + id, name).putString("path." + id, path).commit())
            throw new IOException("Cannot save mod metadata");
    }
    static void open(Activity a) { a.runOnUiThread(() -> { if (!deleting && !a.isFinishing() && !a.isDestroyed()) show(a); }); }
    private static void show(Activity a) {
        File[] dirs = root(a).listFiles();
        ArrayList<String> ids = new ArrayList<>(), labels = new ArrayList<>(), errors = new ArrayList<>();
        if (dirs != null) {
            Arrays.sort(dirs);
            for (File dir : dirs) {
                String id = dir.getName();
                try { if (!UUID.fromString(id).toString().equals(id) || !dir.isDirectory()) continue; }
                catch (IllegalArgumentException e) { continue; }
                String error = "";
                try { resolve(a, id); } catch (IOException e) { error = e.getMessage(); }
                ids.add(id); errors.add(error);
                labels.add(label(a, id) + (error.isEmpty() ? "" : " (unavailable)"));
            }
        }
        String selected = state(a).getString("selected", "");
        String message = "Experimental: engine and game version compatibility are not validated. ExeFS/code patches are not applied.\n\n"
            + "Applied this launch: " + (applied.isEmpty() ? "none" : label(a, applied))
            + "\nSelected for next launch: " + (selected.isEmpty() ? "none" : label(a, selected));
        if (!selected.equals(applied)) message += "\nRestart the game to apply the selection.";
        if (conflict) message += "\nExisting RomFS overlay prevents mod activation.";
        if (ids.isEmpty()) message += "\n\nImport a ZIP first.";
        AlertDialog.Builder dialog = new AlertDialog.Builder(a).setTitle("Manage mods").setPositiveButton("Done", null);
        if (ids.isEmpty() && selected.isEmpty()) { dialog.setMessage(message).show(); return; }
        float density = a.getResources().getDisplayMetrics().density;
        int padding = (int)(20 * density);
        LinearLayout content = new LinearLayout(a);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(padding, padding / 2, padding, padding / 2);
        TextView text = new TextView(a);
        text.setText(message);
        content.addView(text);
        String[] choices = new String[ids.size() + 1]; choices[0] = "Off";
        for (int i = 0; i < ids.size(); i++) choices[i + 1] = labels.get(i);
        int checked = ids.indexOf(selected) + 1;
        ArrayList<RadioButton> buttons = new ArrayList<>();
        AlertDialog shown = dialog.create();
        for (int i = 0; i < choices.length; i++) {
            LinearLayout row = new LinearLayout(a);
            RadioButton button = new RadioButton(a);
            button.setId(ViewGroup.generateViewId());
            button.setText(choices[i]);
            button.setChecked(i == checked);
            buttons.add(button);
            row.addView(button, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            if (i > 0) {
                String mod = ids.get(i - 1);
                Button remove = new Button(a);
                remove.setText("Delete");
                remove.setOnClickListener(v -> {
                    if (mod.equals(applied)) {
                        new AlertDialog.Builder(a).setMessage("Select Off and restart before deleting this mod.").setPositiveButton("OK", null).show();
                        return;
                    }
                    new AlertDialog.Builder(a).setTitle("Delete mod")
                        .setMessage("Delete " + label(a, mod) + "?")
                        .setNegativeButton("Cancel", null)
                        .setPositiveButton("Delete", (d, w) -> {
                            shown.dismiss();
                            deleteMod(a, mod);
                        }).show();
                });
                row.addView(remove);
            }
            content.addView(row);
        }
        ScrollView scroll = new ScrollView(a);
        scroll.addView(content);
        shown.setView(scroll);
        shown.show();
        for (int i = 0; i < buttons.size(); i++) {
            final int which = i;
            buttons.get(i).setOnClickListener(v -> {   // attached after the initial check: opening the dialog selects nothing
                if (which > 0 && !errors.get(which - 1).isEmpty()) {
                    shown.dismiss();
                    new AlertDialog.Builder(a).setMessage("Cannot enable: " + errors.get(which - 1)).setPositiveButton("OK", (x, y) -> show(a)).show();
                    return;
                }
                String choice = which == 0 ? "" : ids.get(which - 1);
                if (!state(a).edit().putString("selected", choice).commit()) {
                    shown.dismiss();
                    new AlertDialog.Builder(a).setMessage("Could not save selection.").setPositiveButton("OK", (x, y) -> show(a)).show();
                    return;
                }
                shown.dismiss(); show(a);
            });
        }
    }
    private static void deleteMod(Activity a, String id) {
        if (deleting) return;
        deleting = true;
        AlertDialog progress = new AlertDialog.Builder(a).setMessage("Deleting mod...").setCancelable(false).show();
        Context context = a.getApplicationContext();
        new Thread(() -> {
            String error = "";
            try {
                if (id.equals(applied)) throw new IOException("Select Off and restart before deleting this mod.");
                try { if (!UUID.fromString(id).toString().equals(id)) throw new IllegalArgumentException(); }
                catch (IllegalArgumentException e) { throw new IOException("Invalid mod ID"); }
                File external = context.getExternalFilesDir(null).getCanonicalFile();
                File base = new File(external, "usermods"), dir = new File(base, id);
                if (!base.getCanonicalPath().equals(base.getAbsolutePath())
                        || !dir.getCanonicalPath().equals(dir.getAbsolutePath()) || !dir.isDirectory())
                    throw new IOException("Missing or unsafe mod directory");
                SharedPreferences prefs = state(context);
                if (id.equals(prefs.getString("selected", "")) && !prefs.edit().putString("selected", "").commit())
                    throw new IOException("Cannot clear selection");
                Files.walkFileTree(dir.toPath(), new SimpleFileVisitor<Path>() {
                    @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) throws IOException {
                        Files.delete(file); return FileVisitResult.CONTINUE;
                    }
                    @Override public FileVisitResult postVisitDirectory(Path directory, IOException failure) throws IOException {
                        if (failure != null) throw failure;
                        Files.delete(directory); return FileVisitResult.CONTINUE;
                    }
                });
                if (!prefs.edit().remove("name." + id).remove("path." + id).commit())
                    throw new IOException("Cannot remove mod metadata");
                Log.i("dmhost", "[mod] deleted " + id);
            } catch (Exception e) {
                error = e.toString();
                Log.e("dmhost", "[RED] [mod] delete failed: " + error);
            }
            String result = error;
            a.runOnUiThread(() -> {
                deleting = false;
                if (a.isFinishing() || a.isDestroyed()) return;
                progress.dismiss();
                if (!result.isEmpty()) new AlertDialog.Builder(a).setMessage("Delete failed: " + result)
                    .setPositiveButton("OK", (d, w) -> show(a)).show();
                else show(a);
            });
        }, "mod-delete").start();
    }
    private static String label(Context c, String id) { return state(c).getString("name." + id, "Mod " + id.substring(0, 8)); }
}
