package org.nx2a;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.HashSet;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import org.json.JSONObject;

// The launcher imports into a private staging tree; the native host starts only after it is complete.
public class ImportActivity extends Activity {
    private File root;
    private String dataPackage, version;
    private TextView status;
    private Button choose;
    private ProgressBar progress;
    private boolean importing;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        root = getExternalFilesDir(null);
        if (root == null) { finish(); return; }
        try {
            dataPackage = getPackageManager().getActivityInfo(getComponentName(), 128).metaData.getString("nx2a.data_package");
            version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
            File imported = new File(root, ".nx2a-imported");
            if (ready(new File(imported, ".ready").isFile() ? imported : root)) { launch(); return; }
        } catch (Exception e) {
            android.util.Log.e("nx2a-import", "Cannot inspect installed data", e);
        }
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        int pad = (int)(24 * getResources().getDisplayMetrics().density);
        layout.setPadding(pad, pad, pad, pad);
        TextView title = new TextView(this);
        title.setText(getApplicationInfo().loadLabel(getPackageManager()));
        title.setTextSize(24);
        layout.addView(title);
        status = new TextView(this);
        status.setText("Choose the game's complete ZIP. No manual copying into Android/data is needed.\n\nKeep the ZIP until import finishes. You need enough free space for both the ZIP and its extracted data. Saves are kept separately.");
        layout.addView(status);
        progress = new ProgressBar(this);
        progress.setVisibility(android.view.View.GONE);
        layout.addView(progress);
        choose = new Button(this);
        choose.setText("Import game ZIP");
        choose.setOnClickListener(v -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            startActivityForResult(pick, 1);
        });
        layout.addView(choose);
        setContentView(layout);
    }

    private boolean ready(File dir) throws Exception {
        File manifest = new File(dir, "nx2a-data.json");
        if (!manifest.isFile() || manifest.length() > 65536) return false;
        JSONObject info = new JSONObject(new String(Files.readAllBytes(manifest.toPath()), StandardCharsets.UTF_8));
        return dataPackage.equals(info.getString("package")) && version.equals(info.getString("display"))
            && new File(dir, "mods/main.patched.bin").length() > 0
            && new File(dir, "mods/main.fixups.tsv").length() > 0
            && new File(dir, "shaders.dmsv").length() > 0 && new File(dir, "RomFS").isDirectory();
    }

    private void launch() {
        startActivity(new Intent().setClassName(this, "android.app.NativeActivity"));
        finish();
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != 1 || result != RESULT_OK || data == null || importing) return;
        Uri uri = data.getData();
        importing = true;
        choose.setEnabled(false);
        progress.setVisibility(android.view.View.VISIBLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        new Thread(() -> {
            try {
                extract(uri);
                runOnUiThread(() -> { importing = false; launch(); });
            } catch (Exception e) {
                android.util.Log.e("nx2a-import", "Import failed", e);
                runOnUiThread(() -> {
                    importing = false;
                    status.setText("Import failed: " + e.getMessage() + "\n\nYour saves are unchanged. Select the complete ZIP to retry.");
                    choose.setEnabled(true);
                    progress.setVisibility(android.view.View.GONE);
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                });
            }
        }, "nx2a-import").start();
    }

    private void extract(Uri uri) throws Exception {
        File staging = new File(root, ".nx2a-importing"), target = new File(root, ".nx2a-imported");
        remove(staging);
        if (!staging.mkdir()) throw new IOException("Cannot create import folder");
        String prefix = dataPackage + "/files/", base = staging.getCanonicalPath() + "/";
        long bytes = 0, shown = 0, budget = staging.getUsableSpace() - (256L << 20);
        HashSet<String> names = new HashSet<>();
        byte[] buffer = new byte[1 << 20];
        try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(getContentResolver().openInputStream(uri), 1 << 20))) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                String name = entry.getName();
                if (entry.isDirectory()) continue;
                if (!name.startsWith(prefix)) {
                    // The complete distribution also contains its APK; it is not game data.
                    if (!name.contains("/") && name.endsWith(".apk")) continue;
                    throw new IOException("Wrong archive layout: " + name);
                }
                String rel = name.substring(prefix.length());
                File out = new File(staging, rel);
                if (rel.isEmpty() || rel.contains("\\") || rel.startsWith("/") || !out.getCanonicalPath().startsWith(base)
                    || !names.add(out.getCanonicalPath()) || names.size() > 100000 || rel.equals(".ready"))
                    throw new IOException("Unsafe or duplicate ZIP path");
                File parent = out.getParentFile();
                if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create " + rel);
                try (FileOutputStream stream = new FileOutputStream(out)) {
                    int n;
                    while ((n = zip.read(buffer)) != -1) {
                        bytes += n;
                        if (bytes > budget) throw new IOException("Not enough free storage for this import");
                        stream.write(buffer, 0, n);
                        if (bytes - shown >= 32L << 20) {
                            shown = bytes;
                            String text = String.format(Locale.US, "Importing %.2f GiB\n%s\n\nKeep this screen open until import finishes.", bytes / 1073741824.0, rel);
                            runOnUiThread(() -> status.setText(text));
                        }
                    }
                }
                zip.closeEntry();   // verifies the entry's CRC, including ZIP64 entries larger than 4 GiB
            }
        }
        if (!ready(staging)) throw new IOException("This ZIP is incomplete or belongs to another game/version");
        File manifest = new File(staging, "nx2a-data.json");
        JSONObject info = new JSONObject(new String(Files.readAllBytes(manifest.toPath()), StandardCharsets.UTF_8));
        if (names.size() - 1 != info.getInt("files") || bytes - manifest.length() != info.getLong("bytes"))
            throw new IOException("The ZIP does not contain all expected data");
        // Absolute paths in env.txt belong to the original manual-install layout, not the imported tree.
        File env = new File(staging, "env.txt");
        if (env.length() > 65536) throw new IOException("Invalid environment file");
        String settings = new String(Files.readAllBytes(env.toPath()), StandardCharsets.UTF_8);
        settings = settings.replace("/sdcard/Android/data/" + dataPackage + "/files", target.getAbsolutePath());
        Files.write(env.toPath(), settings.getBytes(StandardCharsets.UTF_8));
        if (!new File(staging, ".ready").createNewFile()) throw new IOException("Cannot finish import");
        // Only the importer-owned tree is replaced. Legacy data and internal save files are never removed.
        remove(target);
        if (!staging.renameTo(target)) throw new IOException("Cannot activate imported data");
    }

    private void remove(File file) throws IOException {
        if (!file.exists()) return;
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children == null) throw new IOException("Cannot read import folder");
            for (File child : children) remove(child);
        }
        if (!file.delete()) throw new IOException("Cannot remove incomplete import");
    }

    @Override public void onBackPressed() {
        if (!importing) super.onBackPressed();
    }
}
