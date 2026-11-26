package org.nx2a;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.graphics.Point;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileOutputStream;
import java.io.IOException;

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
    private SurfaceView view;

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
        view = null;
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
        Point displaySize = new Point(); selected.getRealSize(displaySize);
        int[] size = canvasSize(activity, displaySize.x, displaySize.y);
        if (size != null) view.getHolder().setFixedSize(size[0], size[1]);
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
        this.view = view;
        displayId = selected.getDisplayId();
        try { next.show(); }
        catch (RuntimeException e) { dismiss(); }
    }

    static String readCanvas(Activity activity) {
        File root = activity.getExternalFilesDir(null);
        if (root == null) return null;
        try (BufferedReader in = new BufferedReader(new FileReader(new File(root, "external_canvas.txt")))) {
            return in.readLine();
        } catch (IOException ignored) { return null; }
    }

    static boolean writeCanvas(Activity activity, String value) {
        File root = activity.getExternalFilesDir(null);
        if (root == null) return false;
        try (FileOutputStream out = new FileOutputStream(new File(root, "external_canvas.txt"))) {
            out.write((value + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
            return true;
        } catch (IOException ignored) { return false; }
    }

    static int[] canvasSize(Activity activity, int width, int height) {
        return canvasForDisplay(readCanvas(activity), width, height);
    }

    static int[] canvasForDisplay(String value, int width, int height) {
        int[] requested = parseCanvasSize(value);
        if (requested == null || width <= 0 || height <= 0) return null;
        int h = requested[1];
        int w = Math.max(1, (int)Math.round((double)width * h / height));
        if (w > 4096) { h = Math.max(1, (int)Math.round((double)h * 4096 / w)); w = 4096; }
        return new int[]{w, h};
    }

    void applyCanvas() {
        if (view == null || presentation == null) return;
        Point p = new Point(); presentation.getDisplay().getRealSize(p);
        int[] size = canvasSize(activity, p.x, p.y);
        if (size == null) view.getHolder().setSizeFromLayout();
        else view.getHolder().setFixedSize(size[0], size[1]);
    }

    static int[] parseCanvasSize(String line) {
        if (line == null) return null;
        String[] dims = line.trim().split("x", -1);
        if (dims.length != 2) return null;
        try {
            int w = Integer.parseInt(dims[0]), h = Integer.parseInt(dims[1]);
            return w > 0 && h > 0 && w <= 4096 && h <= 4096 ? new int[]{w, h} : null;
        } catch (NumberFormatException ignored) { return null; }
    }

    @Override public void onDisplayAdded(int id) { update(); }
    @Override public void onDisplayRemoved(int id) { update(); }
    @Override public void onDisplayChanged(int id) { update(); }
}
