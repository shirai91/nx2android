package org.nx2a;

import android.app.NativeActivity;
import android.app.Dialog;
import android.app.AlertDialog;
import android.content.Context;
import android.os.Bundle;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.RadialGradient;
import android.graphics.Shader;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.View;
import android.view.ViewGroup;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import java.util.ArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.HashMap;
import java.util.Map;
import java.io.InputStreamReader;
import java.io.IOException;

/** Experimental dual-screen wrapper. NativeActivity remains the phone's input owner. */
public final class ExternalActivity extends NativeActivity implements ExternalDisplay.Listener {
    static { System.loadLibrary("dmhost"); }
    private static native void externalSurface(Surface surface);
    private static native void externalInput(long buttons, int lx, int ly, int rx, int ry, boolean menu);
    private static native void externalAudio(int deviceId);
    private static native void externalHaptic();
    private static native boolean externalTouch(MotionEvent event);
    private static native float[] externalOverlay(int width, int height);
    private static native String[] externalPanel();
    private ExternalDisplay external;
    private View pad;
    private Dialog phonePad;
    private final ExecutorService audioExecutor = Executors.newSingleThreadExecutor();

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (android.os.Build.VERSION.SDK_INT >= 31) {
            try { pad = new OriginalPad(); } catch (IOException e) { android.util.Log.w("nx2a-dual", "original overlay unavailable", e); }
        }
        if (pad == null) pad = new Pad();
        phonePad = new Dialog(this, android.R.style.Theme_Material_NoActionBar_Fullscreen);
        phonePad.setCancelable(false);
        phonePad.setContentView(pad);
        external = new ExternalDisplay(this, this);
        external.start();
    }
    @Override protected void onDestroy() {
        if (external != null) external.stop();
        if (phonePad != null) phonePad.dismiss();
        audioExecutor.shutdown();
        super.onDestroy();
    }
    @Override protected void onPause() {
        releasePad();
        super.onPause();
    }
    @Override public void surfaceChanged(Surface surface, int width, int height) {
        externalSurface(surface);
        if (!phonePad.isShowing()) phonePad.show();
        phonePad.getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_DIM_BEHIND);
        phonePad.getWindow().setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        android.util.Log.i("nx2a-dual", "external surface " + width + "x" + height + "; phone pad shown");
    }
    @Override public void surfaceLost() {
        releasePad();
        if (phonePad.isShowing()) phonePad.dismiss();
        externalSurface(null);
        audioExecutor.execute(() -> externalAudio(0));
    }

    private void releasePad() {
        if (pad instanceof Pad) ((Pad)pad).release();
        else if (pad instanceof OriginalPad) ((OriginalPad)pad).release();
    }

    private void showAudioMenu() {
        AudioManager manager = (AudioManager)getSystemService(Context.AUDIO_SERVICE);
        if (manager == null) return;
        ArrayList<String> names = new ArrayList<>(); ArrayList<Integer> ids = new ArrayList<>();
        names.add("System default"); ids.add(0);
        for (AudioDeviceInfo device : manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            int type = device.getType(); String name;
            if (type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER) name = "Phone speaker";
            else if (type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO) name = "Bluetooth call · " + device.getProductName();
            else if (type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP || type == AudioDeviceInfo.TYPE_BLE_HEADSET || type == AudioDeviceInfo.TYPE_BLE_SPEAKER) name = "Bluetooth media · " + device.getProductName();
            else if (type == AudioDeviceInfo.TYPE_HDMI || type == AudioDeviceInfo.TYPE_HDMI_ARC || type == AudioDeviceInfo.TYPE_USB_DEVICE || type == AudioDeviceInfo.TYPE_USB_HEADSET) name = "External audio · " + device.getProductName();
            else continue;
            names.add(name); ids.add(device.getId());
        }
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle("Audio output")
            .setItems(names.toArray(new String[0]), (d, which) -> audioExecutor.execute(() -> externalAudio(ids.get(which))))
            .setNegativeButton("Cancel", null).create();
        dialog.setOnShowListener(d -> dialog.getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_DIM_BEHIND));
        dialog.show();
    }

    /** Draw the original game's atlas at the native overlay's live control positions. */
    private final class OriginalPad extends View {
        private final String[] names = {"stick_l", "stick_r", "dpad", "a", "b", "x", "y", "l", "zl", "r", "zr", "minus", "plus", "ls", "rs", "menu"};
        private final OverlayTheme[] themes = new OverlayTheme[2];
        private final Bitmap[] atlases = new Bitmap[2];
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private boolean audioPress;
        private int menuState = -1;

        OriginalPad() throws IOException {
            super(ExternalActivity.this);
            for (int i = 0; i < 2; i++) {
                try (InputStreamReader in = new InputStreamReader(getAssets().open("overlay/theme.tsv"), "UTF-8")) {
                    themes[i] = OverlayTheme.read(in, i == 0 ? "tablet" : "phone");
                }
                try (java.io.InputStream in = getAssets().open("overlay/" + themes[i].image)) {
                    atlases[i] = BitmapFactory.decodeStream(in);
                    if (atlases[i] == null) throw new IOException("Invalid overlay atlas");
                }
            }
        }

        private int control(String name) { for (int i = 0; i < names.length; i++) if (names[i].equals(name)) return i; return -1; }
        private int direction(String name) { return name.equals("right") ? 0 : name.equals("left") ? 1 : name.equals("down") ? 2 : name.equals("up") ? 3 : -1; }
        private void sprite(Canvas canvas, OverlayTheme theme, Bitmap atlas, OverlayTheme.Sprite s, float[] data, int ctl, float dx, float dy) {
            int base = 4 + ctl * 9;
            float k = getHeight() / theme.designHeight * data[1] * data[base + 8];
            float x = data[base + 1] + dx + s.x * k, y = data[base + 2] + dy + s.y * k;
            paint.setAlpha(Math.round(255 * Math.max(0, Math.min(1, data[2] * theme.alpha))));
            canvas.drawBitmap(atlas, new Rect((int)s.u, (int)s.v, (int)(s.u+s.width), (int)(s.v+s.height)),
                new RectF(x, y, x+s.width/theme.scale*k, y+s.height/theme.scale*k), paint);
        }
        @Override protected void onDraw(Canvas canvas) {
            canvas.drawColor(Color.rgb(15,18,22));
            float[] data = externalOverlay(getWidth(), getHeight());
            if (data == null || data.length != 5 + names.length * 9) return;
            int layout = (int)data[0]; if (layout < 0 || layout >= themes.length) return;
            OverlayTheme theme = themes[layout]; Bitmap atlas = atlases[layout];
            int menu = (int)data[3]; menuState = menu; boolean edit = menu >= 0 && (menu & 0x20000) != 0 && (menu & 0x10000) == 0;
            if (menu >= 0 && !edit) { drawMenu(canvas, data); postInvalidateDelayed(33); return; }
            for (int pass = 0; pass < 2; pass++) for (OverlayTheme.Sprite s : theme.sprites) {
                int ctl = control(s.control); if (ctl < 0) continue;
                int visible = edit ? 1 : (int)data[4 + names.length * 9];
                if (visible == 0 || (visible == 2 && ctl != names.length - 1)) continue;
                boolean decor = s.state.equals("decor"); if (decor != (pass == 0)) continue;
                int base = 4 + ctl * 9, kind = (int)data[base], held = (int)data[base + 5];
                int dir = direction(s.direction);
                boolean on = kind == 3 ? dir >= 0 && (held & (1 << dir)) != 0 : held != 0;
                if (!decor && !s.state.equals("knob") && !s.state.equals(on ? "held" : "idle")) continue;
                float dx = 0, dy = 0;
                if (s.state.equals("knob")) {
                    float k = getHeight() / theme.designHeight * data[1] * data[base + 8];
                    dx = data[base + 6] * theme.travel * k; dy = data[base + 7] * theme.travel * k;
                }
                sprite(canvas, theme, atlas, s, data, ctl, dx, dy);
            }
            if (edit) drawEditor(canvas, data, menu);
            else {
                paint.setAlpha(255); paint.setColor(Color.WHITE); paint.setTextAlign(Paint.Align.CENTER);
                paint.setTextSize(getHeight() * .035f); canvas.drawText("Audio", getWidth() * .62f, getHeight() * .06f, paint);
            }
            postInvalidateDelayed(33);
        }
        private void drawMenu(Canvas c, float[] data) {
            float W=getWidth(), H=getHeight(), scale=Math.min(data[1], .92f/(6*.115f+2*.03f));
            float w=Math.min(.95f*H*scale,W*.96f), h=(6*.115f+2*.03f)*H*scale, x=(W-w)/2, y=(H-h)/2, unit=h/(6*.115f+2*.03f);
            int state=(int)data[3], tab=(state>>8)&1, opacity=(state>>9)&127;
            String[] rows=tab==0 ? new String[]{"Gamepad     Game", "Gamepad: " + ((state&1)!=0?"On":"Off"), "Opacity: "+opacity+"%", "Edit layout", "Rumble: "+((state&2)!=0?"On":"Off"), "Resume"} :
                new String[]{"Gamepad     Game", "FPS: "+((state&8)!=0?"On":"Off"), "Mode: "+((state&4)!=0?"Docked":"Handheld"), "Export save", "Import save", "Resume"};
            if ((state&0x40000)!=0) { String[] saved=externalPanel(); if(saved!=null && saved.length==6) rows=saved; }
            paint.setColor(Color.rgb(35,38,44)); paint.setAlpha(245); c.drawRoundRect(x,y,x+w,y+h,12,12,paint);
            paint.setAlpha(255); paint.setColor(Color.WHITE); paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(H*.032f);
            if ((state&0x10000)!=0) {
                String[] text=externalPanel(); if(text==null) return;
                c.drawText(text[0],x+w/2,y+h*.11f,paint); c.drawText(text[1],x+w/2,y+h*.24f,paint);
                String[] keys={"1234567890","qwertyuiop","asdfghjkl'","zxcvbnm-._"}; boolean shift="1".equals(text[2]);
                for(int row=0;row<4;row++) for(int col=0;col<10;col++) {
                    String k=keys[row].substring(col,col+1); if(shift) k=k.toUpperCase(java.util.Locale.ROOT);
                    c.drawText(k,x+w*(.03f+.94f*(col+.5f)/10),y+h*(.30f+(row+.7f)*.60f/5),paint);
                }
                String[] wide={"Shift","Space","Del","OK"}; float[] centers={.10f,.425f,.725f,.90f};
                for(int i=0;i<4;i++) c.drawText(wide[i],x+w*(.03f+.94f*centers[i]),y+h*(.30f+4.7f*.60f/5),paint);
                return;
            }
            for (int i=0;i<6;i++) c.drawText(rows[i],x+w/2,y+(.03f+(i+.62f)*.115f)*unit,paint);
        }
        private void drawEditor(Canvas c, float[] data, int state) {
            float W=getWidth(), H=getHeight(), scale=Math.min(data[1], .92f/(6*.115f+2*.03f));
            float w=Math.min(.85f*H*scale,W*.96f), h=.30f*H*scale, x=(W-w)/2, y=.13f*H;
            paint.setColor(Color.rgb(35,38,44)); paint.setAlpha(235); c.drawRoundRect(x,y,x+w,y+h,10,10,paint);
            paint.setAlpha(255); paint.setColor(Color.WHITE); paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(H*.026f);
            String[] labels={"−", "+", "Reset", "Cancel", "Save"}; float[] centers={.065f,.18f,.395f,.64f,.8775f};
            for(int i=0;i<labels.length;i++) c.drawText(labels[i],x+w*centers[i],y+h*.51f,paint);
            c.drawText((state&4)!=0?"Snap to grid: On":"Snap to grid: Off",x+w/2,y+h*.88f,paint);
            if ((state&1)!=0) { float pw=Math.min(.75f*H*scale,W*.9f), ph=.30f*H*scale, px=(W-pw)/2, py=(H-ph)/2;
                paint.setColor(Color.DKGRAY); c.drawRect(px,py,px+pw,py+ph,paint); paint.setColor(Color.WHITE);
                c.drawText("Discard changes?",W/2,py+ph*.30f,paint); c.drawText("Discard",px+pw*.265f,py+ph*.72f,paint); c.drawText("Keep",px+pw*.735f,py+ph*.72f,paint); }
        }
        @Override public boolean onTouchEvent(MotionEvent event) {
            int action=event.getActionMasked();
            if (menuState < 0 && action==MotionEvent.ACTION_DOWN && event.getX() > getWidth()*.58f && event.getX() < getWidth()*.66f && event.getY() < getHeight()*.10f) { audioPress=true; return true; }
            if (audioPress) { if (action==MotionEvent.ACTION_UP) { audioPress=false; showAudioMenu(); } else if (action==MotionEvent.ACTION_CANCEL) audioPress=false; return true; }
            boolean ok = externalTouch(event); invalidate(); return ok;
        }
        void release() {
            long now=android.os.SystemClock.uptimeMillis(); MotionEvent cancel=MotionEvent.obtain(now,now,MotionEvent.ACTION_CANCEL,0,0,0);
            externalTouch(cancel); cancel.recycle(); audioPress=false;
        }
    }

    private final class Pad extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Map<Integer,Integer> captured = new HashMap<>();
        private final Map<Integer,float[]> points = new HashMap<>();
        private final String[] labels = {"L", "ZL", "R", "ZR", "−", "+", "L3", "R3", "A", "B", "X", "Y", "✥", "↑", "→", "↓", "MENU", "L", "R", "Audio"};
        private final long[] bits = {1L<<6,1L<<8,1L<<7,1L<<9,1L<<11,1L<<10,1L<<4,1L<<5,1L,1L<<1,1L<<2,1L<<3,0,0,0,0,0,0,0,0};
        private final float[] xs = {.20f,.07f,.80f,.93f,.43f,.57f,.28f,.72f,.88f,.82f,.82f,.76f,.25f,.25f,.30f,.25f,.5f,.11f,.65f,.50f};
        private final float[] ys = {.19f,.19f,.19f,.19f,.10f,.10f,.45f,.45f,.62f,.76f,.48f,.62f,.76f,.65f,.76f,.87f,.10f,.57f,.78f,.27f};
        Pad() { super(ExternalActivity.this); setFocusable(true); setWillNotDraw(false); setLayerType(View.LAYER_TYPE_SOFTWARE, null); }
        @Override protected void onDraw(Canvas c) {
            c.drawColor(Color.rgb(15,18,22)); float w=getWidth(), h=getHeight(), r=Math.min(w,h)*.064f;
            paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(r*.8f); paint.setStrokeWidth(Math.max(2,r*.06f));
            for(int i=0;i<labels.length;i++) {
                if(i>=13 && i<=15) continue;
                float x=xs[i]*w,y=ys[i]*h, rad=(i==12 || i==17 || i==18 ? r*1.65f:r);
                paint.setStyle(Paint.Style.FILL); paint.setColor(Color.WHITE);
                boolean pressed=captured.containsValue(i);
                paint.setShader(new RadialGradient(x,y,rad,pressed ? new int[]{Color.rgb(170,200,220),Color.rgb(130,165,190),Color.rgb(85,110,130)} : new int[]{Color.rgb(226,226,226),Color.rgb(186,188,190),Color.rgb(125,127,130)},null,Shader.TileMode.CLAMP));
                c.drawCircle(x,y,rad,paint); paint.setShader(null);
                paint.setStyle(Paint.Style.STROKE); paint.setColor(Color.DKGRAY); c.drawCircle(x,y,rad,paint);
                paint.setStyle(Paint.Style.FILL); paint.setColor(Color.rgb(35,35,35));
                paint.setTextSize(r*(labels[i].length()>2 ? .48f : .8f));
                if(i==12) { c.drawText("↑",x,y-rad*.48f+r*.29f,paint); c.drawText("↓",x,y+rad*.48f+r*.29f,paint);
                    c.drawText("←",x-rad*.48f,y+r*.29f,paint); c.drawText("→",x+rad*.48f,y+r*.29f,paint); }
                else if (i==17 || i==18) {
                    float dx=0,dy=0;
                    for(Map.Entry<Integer,Integer> e:captured.entrySet()) if(e.getValue()==i) { float[] p=points.get(e.getKey()); if(p!=null){dx=p[0]-x;dy=p[1]-y;} break; }
                    float length=(float)Math.hypot(dx,dy), limit=rad*.55f; if(length>limit){dx*=limit/length;dy*=limit/length;}
                    paint.setColor(Color.argb(110,55,55,55)); c.drawCircle(x+dx,y+dy,rad*.35f,paint);
                    paint.setColor(Color.WHITE); paint.setTextSize(r*.48f); c.drawText(labels[i],x+dx,y+dy+r*.17f,paint);
                } else c.drawText(labels[i],x,y+r*.29f,paint);
            }
        }
        private int hit(float x,float y) {
            float w=getWidth(),h=getHeight(),r=Math.min(w,h)*.064f, best=Float.MAX_VALUE; int which=-1;
            for(int i=0;i<labels.length;i++) { float dx=x-xs[i]*w,dy=y-ys[i]*h,lim=r*(i>=17?1.65f:1.15f),d=dx*dx+dy*dy;
                if(i>=13 && i<=15) continue;
                if(i==12) lim=r*1.65f;
                if(i==19) lim=r*1.15f;
                if(d<=lim*lim && d<best){best=d;which=i;} }
            return which;
        }
        @Override public boolean onTouchEvent(MotionEvent e) {
            int action=e.getActionMasked(), ai=e.getActionIndex(), id=e.getPointerId(ai);
            if(action==MotionEvent.ACTION_DOWN || action==MotionEvent.ACTION_POINTER_DOWN) {
                int control=hit(e.getX(ai),e.getY(ai)); if(control>=0) captured.put(id,control);
                if(control==12 || control>=17) externalHaptic();
                if(control==16) externalInput(0,0,0,0,0,true);
                if(control==19) showAudioMenu();
            }
            for(int i=0;i<e.getPointerCount();i++) points.put(e.getPointerId(i),new float[]{e.getX(i),e.getY(i)});
            if(action==MotionEvent.ACTION_UP || action==MotionEvent.ACTION_POINTER_UP) {captured.remove(id);points.remove(id);}
            if(action==MotionEvent.ACTION_CANCEL){release();return true;}
            long b=0; int[] axes={0,0,0,0}; float radius=Math.min(getWidth(),getHeight())*.1056f;
            for(Map.Entry<Integer,Integer> entry:captured.entrySet()) { int control=entry.getValue(); b|=bits[control];
                if(control==12) {float[] p=points.get(entry.getKey()); if(p==null)continue; float dx=(p[0]-xs[control]*getWidth())/radius,dy=(p[1]-ys[control]*getHeight())/radius;
                    b|=PadMath.direction(dx,dy); }
                if(control==17 || control==18) {float[] p=points.get(entry.getKey()); if(p==null)continue; float dx=(p[0]-xs[control]*getWidth())/radius,dy=(p[1]-ys[control]*getHeight())/radius;
                    float length=(float)Math.hypot(dx,dy); if(length>1){dx/=length;dy/=length;} int k=(control-17)*2; axes[k]=(int)(dx*32767);axes[k+1]=(int)(-dy*32767); }
            }
            externalInput(b,axes[0],axes[1],axes[2],axes[3],false); invalidate();return true;
        }
        void release(){captured.clear();points.clear();externalInput(0,0,0,0,0,false);invalidate();}
    }
}
