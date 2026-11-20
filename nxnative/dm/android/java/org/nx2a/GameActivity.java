package org.nx2a;

import android.app.NativeActivity;
import android.content.Intent;
import java.nio.charset.StandardCharsets;

/** System text input and document import for ordinary single-screen games. */
public class GameActivity extends NativeActivity {
    private final GameKeyboard keyboard = new GameKeyboard(this);
    private final GameFiles files = new GameFiles(this);

    public void importLosslessDll() { files.openLossless(); }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        if (!files.result(request, result, data)) super.onActivityResult(request, result, data);
    }

    private static String text(byte[] value) {
        return value == null ? "" : new String(value, StandardCharsets.UTF_8);
    }

    // Guest callers wait off the UI thread. Ordinary UTF-8, not JNI modified UTF-8.
    public byte[] readKeyboard(byte[] title, byte[] subtitle, byte[] guide, byte[] initial, int min, int max) {
        return keyboard.read(text(title), text(subtitle), text(guide), text(initial), min, max);
    }

    @Override protected void onResume() { super.onResume(); keyboard.pause(false); }
    @Override protected void onPause() { keyboard.pause(true); super.onPause(); }
    @Override protected void onDestroy() { files.close(); keyboard.pause(true); super.onDestroy(); }
}
