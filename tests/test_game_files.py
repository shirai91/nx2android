"""Exercise the production DLL importer with a real JVM/JNI bridge and fake document-provider APIs."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GameFiles(unittest.TestCase):
    @unittest.skipIf(os.name == 'nt', 'the host JNI probe uses a POSIX C compiler')
    def test_native_binding_and_atomic_import_failures(self):
        cc, javac, java = (shutil.which(x) for x in ('cc', 'javac', 'java'))
        if not all((cc, javac, java)):
            self.skipTest('C compiler and JDK required')
        props = subprocess.run([java, '-XshowSettings:properties', '-version'], capture_output=True, text=True)
        match = re.search(r'^\s*java.home = (.+)$', props.stderr, re.M)
        if not match or not (Path(match[1]) / 'include/jni.h').is_file():
            self.skipTest('JDK JNI headers required')
        jdk = Path(match[1])
        sources = {
            'android/app/Activity.java': '''package android.app;
import android.content.*; import java.io.File;
public class Activity {
 public static final int RESULT_OK=-1; public File dir; public int request; public volatile String toast;
 public final ContentResolver resolver=new ContentResolver();
 public void runOnUiThread(Runnable r){r.run();} public boolean isFinishing(){return false;} public boolean isDestroyed(){return false;}
 public void startActivityForResult(Intent i,int request){this.request=request;}
 public File getExternalFilesDir(String s){return dir;} public ContentResolver getContentResolver(){return resolver;}
}''',
            'android/content/Intent.java': '''package android.content; import android.net.Uri;
public class Intent {
 public static final String ACTION_OPEN_DOCUMENT="open", CATEGORY_OPENABLE="openable"; private Uri uri;
 public Intent(String action){} public Intent addCategory(String s){return this;} public Intent setType(String s){return this;}
 public Intent setData(Uri uri){this.uri=uri;return this;} public Uri getData(){return uri;}
}''',
            'android/content/ContentResolver.java': '''package android.content; import java.io.*; import android.net.Uri;
public class ContentResolver { public boolean denied;
 public InputStream openInputStream(Uri uri)throws IOException {if(denied)throw new SecurityException("denied");return new FileInputStream(uri.path);}
}''',
            'android/net/Uri.java': '''package android.net; public class Uri {public final String path; public Uri(String p){path=p;}}''',
            'android/system/Os.java': '''package android.system; import java.nio.file.*; import java.io.IOException;
public class Os {public static void rename(String a,String b)throws IOException{Files.move(Paths.get(a),Paths.get(b),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);}}''',
            'android/widget/Toast.java': '''package android.widget; import android.app.Activity;
public class Toast {public static final int LENGTH_LONG=1; public static Toast makeText(Activity a,String s,int n){a.toast=s;return new Toast();} public void show(){}}''',
            'android/util/Log.java': '''package android.util; public class Log {public static int w(String tag,String text,Throwable e){return 0;}}''',
            'org/nx2a/FilesTest.java': '''package org.nx2a;
import android.app.Activity; import android.content.Intent; import android.net.Uri; import java.nio.file.*; import java.util.Arrays;
public class FilesTest {
 static void check(boolean ok){if(!ok)throw new AssertionError();}
 static void copy(GameFiles f,Activity a,Path input,String expected)throws Exception{
  a.toast=null; f.openLossless(); check(f.result(a.request,Activity.RESULT_OK,new Intent("result").setData(new Uri(input.toString()))));
  long end=System.nanoTime()+5_000_000_000L; while(a.toast==null && System.nanoTime()<end)Thread.sleep(5);
  check(a.toast!=null && a.toast.contains(expected));
  try(java.util.stream.Stream<Path> s=Files.list(a.dir.toPath())){check(s.noneMatch(p->p.getFileName().toString().startsWith(".lossless-")));}
 }
 public static void main(String[] args)throws Exception{
  Activity a=new Activity(); a.dir=Files.createDirectory(Paths.get(args[0],"files")).toFile(); GameFiles f=new GameFiles(a);
  Path input=Paths.get(args[0],"input.dll"), target=a.dir.toPath().resolve("Lossless.dll");
  byte[] valid={'M','Z','o','k'};
  try{
   Files.write(input,valid); copy(f,a,input,"imported"); check(Arrays.equals(Files.readAllBytes(target),valid));
   Files.write(input,new byte[]{1,2,3}); copy(f,a,input,"not a supported"); check(Arrays.equals(Files.readAllBytes(target),valid));
   a.resolver.denied=true; copy(f,a,input,"Cannot read"); check(Arrays.equals(Files.readAllBytes(target),valid));
   a.resolver.denied=false; f.openLossless(); check(f.result(a.request,0,null));
   Files.write(input,valid); copy(f,a,input,"imported");
  }finally{f.close();}
 }
}''',
        }
        with tempfile.TemporaryDirectory(prefix='nx2a_files_jni_') as tmp:
            tmp = Path(tmp)
            for name, source in sources.items():
                p = tmp / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(source)
            native = tmp / 'bridge.c'
            native.write_text('''#include <jni.h>
JNIEXPORT jboolean JNICALL Java_org_nx2a_GameFiles_validLossless(JNIEnv *e,jclass c,jbyteArray a){
 (void)c; if((*e)->GetArrayLength(e,a)!=4)return JNI_FALSE; jbyte b[4];(*e)->GetByteArrayRegion(e,a,0,4,b);return b[0]=='M'&&b[1]=='Z'&&b[2]=='o'&&b[3]=='k';}
JNIEXPORT void JNICALL Java_org_nx2a_GameFiles_losslessImported(JNIEnv *e,jclass c){(void)e;(void)c;}
''')
            platform, ext = ('darwin', 'dylib') if sys.platform == 'darwin' else ('linux', 'so')
            subprocess.run([cc, '-shared', '-fPIC', '-I'+str(jdk/'include'), '-I'+str(jdk/'include'/platform), str(native), '-o', str(tmp/f'libdmhost.{ext}')], capture_output=True, text=True, check=True)
            production = ROOT / 'nxnative/dm/android/java/org/nx2a/GameFiles.java'
            subprocess.run([javac, '-d', str(tmp), str(production), *map(str, tmp.rglob('*.java'))], capture_output=True, text=True, check=True)
            result = subprocess.run([java, '-Djava.library.path='+str(tmp), '-cp', str(tmp), 'org.nx2a.FilesTest', str(tmp)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
