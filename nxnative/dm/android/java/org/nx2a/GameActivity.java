package org.nx2a;

import android.app.NativeActivity;
import java.nio.charset.StandardCharsets;

/** Optional Java text entry; the ordinary NativeActivity APK remains code-less. */
public class GameActivity extends NativeActivity {
    private final GameKeyboard keyboard = new GameKeyboard(this);

    private static String text(byte[] value) {
        return value == null ? "" : new String(value, StandardCharsets.UTF_8);
    }

    // Guest callers wait off the UI thread. Ordinary UTF-8, not JNI modified UTF-8.
    public byte[] readKeyboard(byte[] title, byte[] subtitle, byte[] guide, byte[] initial, int min, int max) {
        return keyboard.read(text(title), text(subtitle), text(guide), text(initial), min, max);
    }

    @Override protected void onResume() { super.onResume(); keyboard.pause(false); }
    @Override protected void onPause() { keyboard.pause(true); super.onPause(); }
    @Override protected void onDestroy() { keyboard.pause(true); super.onDestroy(); }
}
