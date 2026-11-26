package org.nx2a;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.text.TextPaint;
import android.view.MotionEvent;
import android.view.View;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** The controller pool and four two-place player slots. The host owns device state. */
final class ControllerAssignmentView extends View {
    interface Host {
        Device[] devices();
        void move(int source, int player, int cell);
        void close();
    }
    static final class Device {
        final int source, player, cell;
        final String name;
        final float x, y;
        final boolean pressed;
        Device(int source, String name, int player, int cell, float x, float y, boolean pressed) {
            this.source = source; this.name = name; this.player = player; this.cell = cell;
            this.x = x; this.y = y; this.pressed = pressed;
        }
    }
    private final Host host;
    private ByteBuffer font;
    private int glyphCount, bitmapStart;
    private final TextPaint paint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
    private final RectF pool = new RectF(), back = new RectF();
    private final RectF[] slots = new RectF[4];
    private int dragged, pointer = -1, poolOffset;
    private float dragX, dragY, dragOffsetX, dragOffsetY, dragWidth, dragHeight;
    private boolean phonePreview;
    private float previewX, previewY, previewOriginX, previewOriginY, previewRadius;

    ControllerAssignmentView(Context context, Host host) {
        super(context); this.host = host;
        for (int i = 0; i < slots.length; i++) slots[i] = new RectF();
        paint.setTypeface(Typeface.MONOSPACE); setFocusable(true);
        try (InputStream in = context.getAssets().open("font/ui.bin"); ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] chunk = new byte[8192]; int n;
            while ((n = in.read(chunk)) != -1) out.write(chunk, 0, n);
            byte[] data = out.toByteArray(); ByteBuffer b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
            if (data.length >= 8 && b.getInt(0) == 0x4650584e) {
                int count = b.getInt(4);
                if (count >= 0 && count <= (data.length - 8) / 12) { font = b; glyphCount = count; bitmapStart = 8 + count * 12; }
            }
        } catch (Exception ignored) { /* A system font remains available when the optional asset is missing. */ }
    }
    private void geometry() {
        float w = getWidth(), h = getHeight(), gap = h * .025f, top = h * .13f, bottom = h * .90f;
        pool.set(w * .04f, top, w * .43f, bottom);
        float x = w * .47f, width = w * .49f, row = (bottom - top - gap * 3) / 4;
        for (int p = 0; p < 4; p++) slots[p].set(x, top + p * (row + gap), x + width, top + p * (row + gap) + row);
        back.set(w * .82f, h * .025f, w * .96f, h * .105f);
    }
    private void box(Canvas c, RectF r, int color, boolean stroke) {
        paint.setColor(color); paint.setStyle(stroke ? Paint.Style.STROKE : Paint.Style.FILL); paint.setStrokeWidth(Math.max(2, getHeight() * .004f));
        c.drawRoundRect(r, getHeight() * .018f, getHeight() * .018f, paint); paint.setStyle(Paint.Style.FILL);
    }
    private int glyph(int codepoint) {
        if (font == null) return -1;
        for (int pass = 0; pass < 2; pass++, codepoint = 0xfffd) {
            int lo = 0, hi = glyphCount;
            while (lo < hi) {
                int mid = (lo + hi) >>> 1, at = 8 + mid * 12, cp = font.getInt(at) >>> 8;
                if (cp == codepoint) return at;
                if (cp < codepoint) lo = mid + 1; else hi = mid;
            }
        }
        return -1;
    }
    private float textWidth(String value, float size) {
        if (font == null) { paint.setTextSize(size); return paint.measureText(value); }
        float width = 0, scale = size / 9f;
        for (int i = 0; i < value.length();) {
            int cp = value.codePointAt(i); i += Character.charCount(cp); int g = glyph(cp);
            if (g >= 0) width += (font.getInt(g) & 255) * scale;
        }
        return width;
    }
    private String fitText(String value, float size, float width) {
        if (textWidth(value, size) <= width) return value;
        String dots = "..."; float available = width - textWidth(dots, size);
        if (available <= 0) return "";
        int end = 0;
        while (end < value.length()) {
            int next = end + Character.charCount(value.codePointAt(end));
            if (textWidth(value.substring(0, next), size) > available) break;
            end = next;
        }
        return value.substring(0, end) + dots;
    }
    private void panel(Canvas c, RectF r, int rim, int face) {
        float radius = Math.min(r.height() * .28f, getHeight() * .045f);
        float inset = Math.max(2f, getHeight() * .006f);
        paint.setStyle(Paint.Style.FILL); paint.setColor(rim); c.drawRoundRect(r, radius, radius, paint);
        RectF inner = new RectF(r.left + inset, r.top + inset, r.right - inset, r.bottom - inset);
        paint.setColor(face); c.drawRoundRect(inner, Math.max(0, radius - inset), Math.max(0, radius - inset), paint);
    }
    private void text(Canvas c, String value, float x, float y, float size, int color) {
        paint.setColor(color); paint.setStyle(Paint.Style.FILL);
        if (font == null) { paint.setTextSize(size); c.drawText(value, x, y, paint); return; }
        float scale = size / 9f;
        for (int i = 0; i < value.length();) {
            int cp = value.codePointAt(i); i += Character.charCount(cp); int g = glyph(cp);
            if (g < 0) continue;
            int packed = font.getInt(g + 4), width = packed & 255, height = font.get(g + 8);
            int dx = font.get(g + 9), dy = font.get(g + 10), stride = (width + 7) / 8;
            int start = bitmapStart + (packed >>> 8);
            if (height >= 0 && start >= bitmapStart && start + height * stride <= font.capacity()) {
                for (int row = 0; row < height; row++) for (int col = 0; col < width; col++)
                    if ((font.get(start + row * stride + col / 8) & (0x80 >>> (col & 7))) != 0) {
                        float px = x + (dx + col) * scale, py = y + (row - height - dy) * scale;
                        c.drawRect(px, py, px + scale, py + scale, paint);
                    }
            }
            x += (font.getInt(g) & 255) * scale;
        }
    }
    private RectF poolCard(int i) {
        float h = pool.height() / 4, top = pool.top + i * h;
        float inset = getHeight() * .014f;
        return new RectF(pool.left + inset, top + inset, pool.right - inset, top + h - inset);
    }
    private RectF cell(int p, int n) {
        RectF s = slots[p]; float inset = getHeight() * .016f;
        float width = (s.width() - s.height() - inset * 3) / 2;
        return new RectF(s.left + inset + n * (width + inset), s.top + inset, s.left + inset + n * (width + inset) + width, s.bottom - inset);
    }
    private int displayCell(Device[] devices, Device d) {
        if (d.cell != 0) return d.cell;
        for (Device other : devices) if (other.player == d.player && other.cell == 1) return 0;
        return 1;
    }
    private int targetCell(Device[] devices, int player, int visualCell) {
        if (visualCell < 0) return -1;
        boolean[] used = new boolean[2];
        for (Device d : devices) if (d.player == player && d.cell >= 0 && d.cell < 2) {
            used[d.cell] = true;
            if (displayCell(devices, d) == visualCell) return d.cell;
        }
        return used[0] ? 1 : 0;
    }
    private void card(Canvas c, RectF r, Device d) {
        float hue = (335f + (d.source - 1) * 137.508f) % 360f;
        int fill = Color.HSVToColor(new float[]{hue, .28f, .20f});
        int edge = Color.HSVToColor(new float[]{hue, .48f, .82f});
        panel(c, r, d.pressed ? Color.WHITE : edge, fill);
        float radius = Math.min(r.height() * .30f, r.width() * .16f), cx = r.right - radius * 1.5f, cy = r.centerY();
        float ax = d.source == 1 && phonePreview ? previewX : d.x, ay = d.source == 1 && phonePreview ? previewY : d.y;
        if (d.pressed) ax += .28f * (float)Math.sin(android.os.SystemClock.uptimeMillis() / 90.0);
        float tipX = cx + Math.max(-1, Math.min(1, ax)) * radius * .50f;
        float tipY = cy - radius * .32f + Math.max(-1, Math.min(1, ay)) * radius * .30f;
        paint.setColor(edge); paint.setStyle(Paint.Style.FILL);
        RectF base = new RectF(cx - radius * .65f, cy + radius * .27f, cx + radius * .65f, cy + radius * .62f);
        c.drawRoundRect(base, radius * .18f, radius * .18f, paint);
        paint.setStrokeWidth(Math.max(2f, radius * .13f)); paint.setStrokeCap(Paint.Cap.ROUND);
        c.drawLine(cx, cy + radius * .30f, tipX, tipY, paint);
        c.drawCircle(tipX, tipY, radius * .36f, paint);
        paint.setColor(Color.WHITE); c.drawCircle(tipX - radius * .10f, tipY - radius * .10f, radius * .075f, paint);
        float size = getHeight() * .034f, inset = getHeight() * .024f;
        String name = fitText(d.name, size, Math.max(0, cx - radius - r.left - 2 * inset));
        text(c, name, r.left + inset, cy + size * .35f, size, Color.WHITE);
    }
    private boolean previewHit(RectF r, Device d, float x, float y) {
        if (d.source != 1) return false;
        float radius = Math.min(r.height() * .30f, r.width() * .16f), cx = r.right - radius * 1.5f, cy = r.centerY();
        if (Math.hypot(x - cx, y - cy) > radius * 1.2f) return false;
        phonePreview = true; previewOriginX = x; previewOriginY = y; previewRadius = radius; previewX = previewY = 0; return true;
    }
    private int poolCount(Device[] devices) {
        int count = 0; for (Device d : devices) if (d.player < 0) count++;
        return count;
    }
    @Override protected void onDraw(Canvas c) {
        super.onDraw(c); geometry(); c.drawColor(Color.rgb(14, 14, 20));
        float h = getHeight(); text(c, "Controller assignment", getWidth() * .04f, h * .085f, h * .045f, Color.WHITE);
        panel(c, back, Color.rgb(100, 100, 110), Color.rgb(48, 48, 56)); text(c, "Back", back.centerX() - textWidth("Back", h * .035f) / 2, back.centerY() + h * .013f, h * .035f, Color.WHITE);
        panel(c, pool, Color.rgb(70, 70, 80), Color.rgb(22, 22, 29));
        Device[] devices = host.devices();
        int poolCount = poolCount(devices);
        poolOffset = Math.min(poolOffset, Math.max(0, poolCount - 4));
        if (poolCount > 4) text(c, "‹    ›", pool.right - h * .14f, pool.top - h * .025f, h * .04f, Color.WHITE);
        for (int p = 0; p < 4; p++) {
            RectF row = slots[p]; panel(c, row, Color.rgb(70, 70, 80), Color.rgb(22, 22, 29));
            RectF label = new RectF(row.right - row.height(), row.top, row.right, row.bottom);
            panel(c, label, Color.rgb(100, 100, 110), Color.rgb(48, 48, 56));
            String title = (p + 1) + "P";
            text(c, title, label.centerX() - textWidth(title, h * .047f) / 2, label.centerY() + h * .016f, h * .047f, Color.WHITE);
        }
        int poolIndex = 0; Device moving = null;
        for (Device d : devices) {
            if (d.source == dragged) moving = d;
            if (d.player < 0) {
                if (poolIndex >= poolOffset && poolIndex < poolOffset + 4 && d.source != dragged) card(c, poolCard(poolIndex - poolOffset), d);
                poolIndex++;
            } else if (d.player < 4 && d.cell >= 0 && d.cell < 2 && d.source != dragged) card(c, cell(d.player, displayCell(devices, d)), d);
        }
        if (dragged != 0) {
            int accent = Color.rgb(255, 60, 142);
            if (pool.contains(dragX, dragY)) box(c, pool, accent, true);
            else for (int p = 0; p < 4; p++) if (slots[p].contains(dragX, dragY)) {
                box(c, slots[p], accent, true);
                for (int n = 0; n < 2; n++) if (cell(p, n).contains(dragX, dragY)) box(c, cell(p, n), accent, true);
            }
            if (moving != null) {
                float left = dragX - dragOffsetX, top = dragY - dragOffsetY;
                card(c, new RectF(left, top, left + dragWidth, top + dragHeight), moving);
            }
        }
        postInvalidateDelayed(50);
    }
    @Override public boolean onTouchEvent(MotionEvent e) {
        geometry(); int action = e.getActionMasked(), index = e.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN) {
            float x = e.getX(), y = e.getY(); if (back.contains(x, y)) { host.close(); return true; }
            Device[] devices = host.devices(); dragged = 0; int poolCount = poolCount(devices);
            if (poolCount > 4 && y >= pool.top - getHeight() * .08f && y < pool.top && x > pool.right - getHeight() * .15f && x < pool.right) {
                poolOffset = Math.max(0, Math.min(poolCount - 4, poolOffset + (x < pool.right - getHeight() * .075f ? -1 : 1)));
                invalidate(); return true;
            }
            int poolIndex = 0;
            for (Device d : devices) {
                RectF r = null;
                if (d.player < 0) {
                    if (poolIndex >= poolOffset && poolIndex < poolOffset + 4 && poolCard(poolIndex - poolOffset).contains(x, y)) r = poolCard(poolIndex - poolOffset);
                    poolIndex++;
                } else if (d.player < 4 && d.cell >= 0 && d.cell < 2 && cell(d.player, displayCell(devices, d)).contains(x, y)) r = cell(d.player, displayCell(devices, d));
                if (r != null) {
                    if (!previewHit(r, d, x, y)) {
                        dragged = d.source; dragOffsetX = x - r.left; dragOffsetY = y - r.top; dragWidth = r.width(); dragHeight = r.height();
                    }
                    break;
                }
            }
            pointer = e.getPointerId(0); dragX = x; dragY = y; invalidate(); return true;
        }
        int i = e.findPointerIndex(pointer);
        if (i < 0) return true;
        dragX = e.getX(i); dragY = e.getY(i);
        if (phonePreview) { previewX = (dragX - previewOriginX) / previewRadius; previewY = (dragY - previewOriginY) / previewRadius; }
        if (action == MotionEvent.ACTION_UP && dragged != 0) {
            if (pool.contains(dragX, dragY)) host.move(dragged, -1, -1);
            else for (int p = 0; p < 4; p++) if (slots[p].contains(dragX, dragY)) {
                int target = cell(p, 0).contains(dragX, dragY) ? 0 : cell(p, 1).contains(dragX, dragY) ? 1 : -1;
                Device[] devices = host.devices();
                // A lone cell-0 card is drawn on the right; move its backing cell before filling the left.
                if (target == 0) for (Device d : devices) if (d.player == p && d.cell == 0 && d.source != dragged && displayCell(devices, d) == 1) {
                    host.move(d.source, p, 1); devices = host.devices(); break;
                }
                host.move(dragged, p, targetCell(devices, p, target)); break;
            }
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) { dragged = 0; pointer = -1; phonePreview = false; }
        invalidate(); return true;
    }
}
