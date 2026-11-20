package org.nx2a;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.widget.Toast;
import android.system.Os;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** User-supplied frame-generation shaders, copied from the system document picker. */
final class GameFiles {
    // NativeActivity's dlopen alone does not register JNI methods with this class loader.
    static { System.loadLibrary("dmhost"); }
    private static final int LOSSLESS_REQUEST = 0x4c53;
    private static final int MAX_DLL_BYTES = 32 * 1024 * 1024;
    private static native boolean validLossless(byte[] data);
    private static native void losslessImported();
    private final Activity activity;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private volatile boolean busy;

    GameFiles(Activity activity) { this.activity = activity; }

    void openLossless() {
        activity.runOnUiThread(() -> {
            if (busy || activity.isFinishing() || activity.isDestroyed()) return;
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            try { busy = true; activity.startActivityForResult(intent, LOSSLESS_REQUEST); }
            catch (RuntimeException e) { busy = false; note("No document picker is available."); }
        });
    }

    boolean result(int request, int code, Intent data) {
        if (request != LOSSLESS_REQUEST) return false;
        Uri uri = code == Activity.RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) { busy = false; return true; }
        worker.execute(() -> importLossless(uri));
        return true;
    }

    private void importLossless(Uri uri) {
        File tmp = null;
        String message = "Unable to import Lossless.dll.";
        try {
            File dir = activity.getExternalFilesDir(null);
            if (dir == null) throw new IOException("no game files folder");
            tmp = File.createTempFile(".lossless-", ".tmp", dir);
            try (InputStream in = activity.getContentResolver().openInputStream(uri);
                 FileOutputStream out = new FileOutputStream(tmp)) {
                if (in == null) throw new IOException("unreadable document");
                byte[] buffer = new byte[65536]; int total = 0, count;
                while ((count = in.read(buffer)) != -1) {
                    if (Thread.currentThread().isInterrupted()) throw new IOException("cancelled");
                    if (count > MAX_DLL_BYTES - total) throw new IOException("DLL exceeds size limit");
                    out.write(buffer, 0, count); total += count;
                }
                out.getFD().sync();
            }
            if (!validLossless(Files.readAllBytes(tmp.toPath()))) {
                message = "This file is not a supported Lossless.dll.";
                throw new IOException("unsupported shader resources");
            }
            if (Thread.currentThread().isInterrupted()) throw new IOException("cancelled");
            // Same-directory rename publishes a complete file and preserves the previous DLL on failure.
            Os.rename(tmp.getAbsolutePath(), new File(dir, "Lossless.dll").getAbsolutePath());
            losslessImported();
            message = "Lossless.dll imported.";
        } catch (SecurityException e) {
            message = "Cannot read this document. Select it again in the system picker.";
            android.util.Log.w("nx2a-files", "Lossless.dll document access failed", e);
        } catch (LinkageError e) {
            message = "Lossless.dll validation is unavailable. Update the APK.";
            android.util.Log.w("nx2a-files", "Lossless.dll native bridge unavailable", e);
        } catch (Exception e) {
            android.util.Log.w("nx2a-files", "Lossless.dll import failed", e);
        } finally {
            if (tmp != null) tmp.delete();
            busy = false;
            note(message);
        }
    }

    private void note(String text) {
        activity.runOnUiThread(() -> {
            if (!activity.isFinishing() && !activity.isDestroyed()) Toast.makeText(activity, text, Toast.LENGTH_LONG).show();
        });
    }

    void close() { worker.shutdownNow(); }
}
