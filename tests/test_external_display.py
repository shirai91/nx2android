"""Exercise external-display attach/detach without an Android device."""
import os
import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'nxnative/dm/android/java/org/nx2a/ExternalDisplay.java'
STUBS = {
    'android/content/Context.java': '''package android.content; public class Context { public static final String DISPLAY_SERVICE="display"; public Object getSystemService(String name) { return null; } }''',
    'android/app/Activity.java': '''package android.app; public class Activity extends android.content.Context { public final android.hardware.display.DisplayManager manager = new android.hardware.display.DisplayManager(); @Override public Object getSystemService(String n) { return manager; } }''',
    'android/os/Looper.java': '''package android.os; public class Looper { public static Looper getMainLooper() { return new Looper(); } }''',
    'android/os/Handler.java': '''package android.os; public class Handler { public Handler(Looper l) {} }''',
    'android/view/Display.java': '''package android.view; public class Display { public static final int INVALID_DISPLAY=-1; private final int id; public Display(int i) { id=i; } public int getDisplayId() { return id; } }''',
    'android/view/Surface.java': '''package android.view; public class Surface {}''',
    'android/view/SurfaceHolder.java': '''package android.view; public interface SurfaceHolder { Surface getSurface(); void addCallback(Callback c); interface Callback { void surfaceCreated(SurfaceHolder h); void surfaceChanged(SurfaceHolder h,int format,int w,int height); void surfaceDestroyed(SurfaceHolder h); } }''',
    'android/view/SurfaceView.java': '''package android.view; public class SurfaceView { public static Holder last; public SurfaceView(android.content.Context c) { last=new Holder(); } public SurfaceHolder getHolder(){ return last; } public static class Holder implements SurfaceHolder { public Callback cb; public final Surface surface=new Surface(); public Surface getSurface(){return surface;} public void addCallback(Callback c){cb=c;} } }''',
    'android/app/Presentation.java': '''package android.app; public class Presentation { public static boolean fail; public Presentation(Activity a,android.view.Display d){} public android.content.Context getContext(){return new android.content.Context();} public void setContentView(android.view.SurfaceView v){} public void show(){if(fail)throw new RuntimeException();} public void dismiss(){} }''',
    'android/hardware/display/DisplayManager.java': '''package android.hardware.display; public class DisplayManager { public static final String DISPLAY_CATEGORY_PRESENTATION="presentation"; public android.view.Display[] displays={}; public int registrations; public interface DisplayListener { void onDisplayAdded(int i); void onDisplayRemoved(int i); void onDisplayChanged(int i); } public void registerDisplayListener(DisplayListener l,android.os.Handler h){registrations++;} public void unregisterDisplayListener(DisplayListener l){registrations--;} public android.view.Display[] getDisplays(String c){return displays;} }''',
    'Check.java': '''import org.nx2a.ExternalDisplay; import android.app.*; import android.view.*;
public class Check {
 public static void main(String[] args) {
  Activity a=new Activity(); int[] events={0,0};
  ExternalDisplay d=new ExternalDisplay(a,new ExternalDisplay.Listener(){public void surfaceChanged(Surface s,int w,int h){if(w!=1920||h!=1080)throw new AssertionError();events[0]++;} public void surfaceLost(){events[1]++;}});
  d.start(); d.start(); if(a.manager.registrations!=1)throw new AssertionError("register");
  a.manager.displays=new Display[]{new Display(7)}; d.onDisplayAdded(7); SurfaceView.Holder old=SurfaceView.last; old.cb.surfaceChanged(old,0,1920,1080);
  a.manager.displays=new Display[]{new Display(8)}; d.onDisplayRemoved(7); if(events[1]!=1)throw new AssertionError("detach");
  old.cb.surfaceChanged(old,0,1920,1080); old.cb.surfaceDestroyed(old); if(events[0]!=1||events[1]!=1)throw new AssertionError("stale callback");
  SurfaceView.Holder fresh=SurfaceView.last; fresh.cb.surfaceChanged(fresh,0,1920,1080); d.stop(); d.stop();
  if(events[0]!=2||events[1]!=2||a.manager.registrations!=0)throw new AssertionError("stop");
  Presentation.fail=true; d.start(); d.stop(); if(a.manager.registrations!=0)throw new AssertionError("failure");
 }
}''',
}

class ExternalDisplayTest(unittest.TestCase):
    def test_optional_port_builds_activity_with_sdk(self):
        from porter import cli, pipeline
        self.assertTrue(cli._parser().parse_args(['port', '/game', '--external-display']).external_display)
        with tempfile.TemporaryDirectory() as t:
            root = pathlib.Path(t); platform = root / 'platforms/android-36/android.jar'; platform.parent.mkdir(parents=True); platform.touch()
            d8 = root / 'build-tools/36.0.0' / ('d8.bat' if os.name == 'nt' else 'd8'); d8.parent.mkdir(parents=True); d8.touch()   # what java_tools looks for
            port = pipeline.Port(pipeline.Options(folder=root, external_display=True)); port.aw = root / 'out'; port.logs = root / 'logs'; port.env = {}
            with mock.patch.object(pipeline.toolchain, '_sdk_roots', return_value=[root]), mock.patch.object(pipeline.shutil, 'which', return_value='/usr/bin/javac'),                  mock.patch.object(pipeline, '_jdk_env', side_effect=lambda javac, env: dict(env or {})), mock.patch.object(pipeline, '_run_java') as run:
                args = port.java_bridge()
            self.assertEqual(run.call_count, 2)
            self.assertEqual(args[-1], 'org.nx2a.ExternalActivity')
            self.assertEqual(args[0], '--dex')

    def test_attach_detach_and_stale_surface(self):
        with tempfile.TemporaryDirectory() as t:
            root = pathlib.Path(t)
            paths = []
            for path, source in STUBS.items():
                p = root / path
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(source)
                paths.append(str(p))
            subprocess.run(['javac', '-d', str(root), str(SOURCE), *paths], check=True)
            subprocess.run(['java', '-cp', str(root), 'Check'], check=True)

    def test_optional_activity_is_packaged_without_changing_default(self):
        with tempfile.TemporaryDirectory() as t:
            root = pathlib.Path(t)
            so, dex, key = root / 'host.so', root / 'classes.dex', root / 'key.json'
            so.write_bytes(b'ELF fixture')
            dex.write_bytes(b'dex fixture')
            tool = ROOT / 'nxnative/dm/android/apk/mkapk.py'
            base = ['python3', str(tool), str(so)]
            subprocess.run([*base, str(root / 'default.apk'), str(key)], check=True, capture_output=True)
            with zipfile.ZipFile(root / 'default.apk') as z:
                self.assertNotIn('classes.dex', z.namelist())
                self.assertIn('android.app.NativeActivity'.encode('utf-16-le'), z.read('AndroidManifest.xml'))
            subprocess.run([*base, str(root / 'custom.apk'), str(key), '--dex', str(dex), '--activity', 'org.nx2a.ExternalActivity'], check=True, capture_output=True)
            with zipfile.ZipFile(root / 'custom.apk') as z:
                self.assertEqual(z.read('classes.dex'), dex.read_bytes())
                self.assertIn('org.nx2a.ExternalActivity'.encode('utf-16-le'), z.read('AndroidManifest.xml'))
            result = subprocess.run([*base, str(root / 'bad.apk'), str(key), '--activity', 'org.nx2a.ExternalActivity'], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
