package org.nx2a;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.UUID;

final class ModManager {
    private static String applied = "";
    private static boolean conflict;

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
    static void open(Activity a) { a.runOnUiThread(() -> { if (!a.isFinishing() && !a.isDestroyed()) show(a); }); }
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
        RadioGroup group = new RadioGroup(a);
        group.setId(ViewGroup.generateViewId());
        String[] choices = new String[ids.size() + 1]; choices[0] = "Off";
        for (int i = 0; i < ids.size(); i++) choices[i + 1] = labels.get(i);
        int checked = ids.indexOf(selected) + 1;
        for (int i = 0; i < choices.length; i++) {
            RadioButton button = new RadioButton(a);
            button.setId(ViewGroup.generateViewId());
            button.setText(choices[i]);
            group.addView(button, new RadioGroup.LayoutParams(RadioGroup.LayoutParams.MATCH_PARENT, RadioGroup.LayoutParams.WRAP_CONTENT));
            if (i == checked) group.check(button.getId());
        }
        content.addView(group);
        ScrollView scroll = new ScrollView(a);
        scroll.addView(content);
        AlertDialog shown = dialog.setView(scroll).show();
        group.setOnCheckedChangeListener((g, id) -> {   // attached after the initial check: opening the dialog selects nothing
            int which = -1;
            for (int i = 0; i < group.getChildCount(); i++) if (group.getChildAt(i).getId() == id) which = i;
            if (which < 0) return;
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
    private static String label(Context c, String id) { return state(c).getString("name." + id, "Mod " + id.substring(0, 8)); }
}
