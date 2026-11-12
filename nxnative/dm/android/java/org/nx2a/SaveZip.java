package org.nx2a;

import java.util.List;
import java.util.Locale;
import java.util.TreeSet;

/** Where a game's save is in an exported .zip (SaveBridge): its entries' names only, so it is tested without Android (tests/test_save_folder.py). */
final class SaveZip {
    private SaveZip() {}

    /** What scan found: prefix, the save's folder in the zip ("<title ID>/", or "<folder>/<title ID>/" when the zip was made of a folder holding it), null none;
     *  files, the files under it; others, the title IDs of other games' save folders; unsafe, a name with a ".." part (the zip is refused) */
    static final class Layout { String prefix; int files; final TreeSet<String> others = new TreeSet<>(); String unsafe; }

    /** An entry's name with / for \ and no leading / */
    static String norm(String n) { n = n.replace('\\', '/'); while (n.startsWith("/")) n = n.substring(1); return n; }

    /** names: the entries' names (norm), a folder's ending in /. The save's folder is matched without regard to case (a PC's zip tool may change it) */
    static Layout scan(List<String> names, String title) {
        Layout l = new Layout();
        for (String n : names) {
            String[] p = n.split("/");
            for (String s : p) if (s.equals("..")) l.unsafe = n;
            if (p[0].equals("__MACOSX")) continue;   // (macOS's zip tool adds it)
            for (int k = 0; k < Math.min(2, p.length - (n.endsWith("/") ? 0 : 1)); k++) {
                if (p[k].equalsIgnoreCase(title)) { if (l.prefix == null) l.prefix = join(p, k + 1); break; }
                String base = p[k].endsWith(".device") ? p[k].substring(0, p[k].length() - 7) : p[k];
                if (base.matches("[0-9A-Fa-f]{16}")) l.others.add(base.toUpperCase(Locale.US));
            }
        }
        l.others.remove(title.toUpperCase(Locale.US));
        if (l.prefix != null) for (String n : names) if (!n.endsWith("/") && n.regionMatches(true, 0, l.prefix, 0, l.prefix.length())) l.files++;
        return l;
    }

    /** name's path under prefix (the save's folder, or its .device folder), null when it is not under it or would leave it */
    static String inside(String name, String prefix) {
        if (!name.regionMatches(true, 0, prefix, 0, prefix.length())) return null;
        String rest = name.substring(prefix.length());
        if (rest.isEmpty()) return null;
        for (String s : rest.split("/")) if (s.equals("..")) return null;
        return rest;
    }

    /** The device save's folder beside the save's: "<title ID>.device/" */
    static String devicePrefix(String prefix) { return prefix.substring(0, prefix.length() - 1) + ".device/"; }

    private static String join(String[] p, int k) { StringBuilder b = new StringBuilder(); for (int i = 0; i < k; i++) b.append(p[i]).append('/'); return b.toString(); }
}
