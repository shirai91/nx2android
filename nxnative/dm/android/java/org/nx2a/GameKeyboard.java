package org.nx2a;

import android.app.AlertDialog;
import android.os.Looper;
import android.text.Editable;
import android.text.InputFilter;
import android.text.TextWatcher;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;

/** A guest can wait for the native input box while the Android UI thread keeps handling lifecycle events. */
final class GameKeyboard {
    private final GameActivity activity;
    private AlertDialog dialog;
    private boolean paused = true;

    GameKeyboard(GameActivity activity) { this.activity = activity; }

    void pause(boolean value) {
        paused = value;
        if (value && dialog != null) dialog.dismiss();
    }

    private static final class Request {
        final CountDownLatch done = new CountDownLatch(1);
        byte[] result;
    }

    byte[] read(String title, String subtitle, String guide, String initial, int min, int max) {
        if (Looper.myLooper() == Looper.getMainLooper()) return null;
        final int limit = max > 0 ? Math.min(max, 1002) : 1002;
        final int minimum = Math.max(0, min);
        if (minimum > limit) return null;
        Request req = new Request();
        activity.runOnUiThread(() -> {
            if (paused || dialog != null || activity.isFinishing() || activity.isDestroyed()) {
                req.done.countDown();
                return;
            }
            try {
                EditText edit = new EditText(activity);
                edit.setSingleLine(true);
                edit.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_ACTION_DONE);
                edit.setFilters(new InputFilter[] {new InputFilter.LengthFilter(limit)});
                edit.setHint(guide);
                edit.setText(initial);
                edit.setSelection(edit.length());
                AlertDialog current = new AlertDialog.Builder(activity)
                    .setTitle(title.isEmpty() ? "Input" : title)
                    .setMessage(subtitle.isEmpty() ? null : subtitle)
                    .setView(edit)
                    .setPositiveButton(android.R.string.ok, (d, button) ->
                        req.result = edit.getText().toString().getBytes(StandardCharsets.UTF_8))
                    .setNegativeButton(android.R.string.cancel, null).create();
                current.setOnDismissListener(d -> {
                    if (dialog == current) dialog = null;
                    req.done.countDown();
                });
                dialog = current;
                current.show();
                current.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE |
                                                     WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
                Runnable validate = () -> current.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(edit.length() >= minimum);
                validate.run();
                edit.addTextChangedListener(new TextWatcher() {
                    @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                    @Override public void onTextChanged(CharSequence s, int start, int before, int count) { validate.run(); }
                    @Override public void afterTextChanged(Editable s) {}
                });
                edit.setOnEditorActionListener((v, action, event) -> {
                    if (action != EditorInfo.IME_ACTION_DONE) return false;
                    if (edit.length() >= minimum) current.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
                    return true;
                });
                edit.requestFocus();
            } catch (RuntimeException e) {
                // The activity lost its window between posting the request and showing it.
                if (dialog != null) { dialog.dismiss(); dialog = null; }
                req.done.countDown();
            }
        });
        boolean interrupted = false;
        for (;;) {
            try { req.done.await(); break; }
            catch (InterruptedException e) { interrupted = true; }
        }
        if (interrupted) Thread.currentThread().interrupt();
        return req.result;
    }
}
