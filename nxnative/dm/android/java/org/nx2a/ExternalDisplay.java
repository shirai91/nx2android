package org.nx2a;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/** Routes an optional presentation surface to the host; all calls run on the UI thread. */
public final class ExternalDisplay implements DisplayManager.DisplayListener {
    public interface Listener {
        void surfaceChanged(Surface surface, int width, int height);
        void surfaceLost();
    }

    private final Activity activity;
    private final DisplayManager displays;
    private final Listener listener;
    private Presentation presentation;
    private int displayId = Display.INVALID_DISPLAY;
    private boolean active;
    private boolean surfaceLive;

    public ExternalDisplay(Activity activity, Listener listener) {
        this.activity = activity;
        this.listener = listener;
        this.displays = (DisplayManager) activity.getSystemService(Context.DISPLAY_SERVICE);
    }

    public void start() {
        if (active || displays == null) return;
        active = true;
        displays.registerDisplayListener(this, new Handler(Looper.getMainLooper()));
        update();
    }

    public void stop() {
        if (!active) return;
        active = false;
        displays.unregisterDisplayListener(this);
        dismiss();
    }

    private void dismiss() {
        Presentation old = presentation;
        presentation = null;
        displayId = Display.INVALID_DISPLAY;
        if (surfaceLive) { surfaceLive = false; listener.surfaceLost(); }
        if (old != null) old.dismiss();
    }

    private void update() {
        if (!active) return;
        Display[] candidates = displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        Display selected = candidates.length == 0 ? null : candidates[0];
        if (selected == null) { dismiss(); return; }
        if (presentation != null && selected.getDisplayId() == displayId) return;
        dismiss();
        Presentation next = new Presentation(activity, selected);
        SurfaceView view = new SurfaceView(next.getContext());
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) { }
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                if (presentation != next || !active) return;
                surfaceLive = true;
                listener.surfaceChanged(holder.getSurface(), width, height);
            }
            @Override public void surfaceDestroyed(SurfaceHolder holder) {
                if (presentation != next || !surfaceLive) return;
                surfaceLive = false;
                listener.surfaceLost();
            }
        });
        next.setContentView(view);
        presentation = next;
        displayId = selected.getDisplayId();
        try { next.show(); }
        catch (RuntimeException e) { dismiss(); }
    }

    @Override public void onDisplayAdded(int id) { update(); }
    @Override public void onDisplayRemoved(int id) { update(); }
    @Override public void onDisplayChanged(int id) { update(); }
}
