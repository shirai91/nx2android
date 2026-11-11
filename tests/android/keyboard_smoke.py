#!/usr/bin/env python3
"""Build an asset-free keyboard contract APK and operate it on a dedicated ARM64 emulator."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from xml.etree import ElementTree

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from porter import pipeline


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk',type=Path,required=True); p.add_argument('--ndk',type=Path,required=True)
    p.add_argument('--without-java',action='store_true',help='verify the code-less APK fallback')
    p.add_argument('--serial',required=True); p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    if not a.serial.startswith('emulator-'): p.error('requires a dedicated emulator')
    a.out.mkdir(parents=True,exist_ok=True); os.environ['ANDROID_HOME']=str(a.sdk)
    dm=ROOT/'nxnative/dm'; host='darwin-x86_64' if sys.platform=='darwin' else 'linux-x86_64'
    source=(dm/'host/hle_swkbd.c').read_text(); start=source.index('enum { ARG_HEADER'); end=source.index('\nHLE', start)
    (a.out/'swkbd_core.inc').write_text(source[start:end])
    cc=a.ndk/'toolchains/llvm/prebuilt'/host/'bin/clang'
    lib=a.out/'libdmhost.so'
    subprocess.run([str(cc),'--target=aarch64-linux-android29','-shared','-fPIC','-Wall','-Werror',
                    *(['-DKEYBOARD_NO_JAVA'] if a.without_java else []),'-I'+str(a.out),'-I'+str(dm/'android'),str(Path(__file__).with_name('keyboard_fixture.c')),
                    '-o',str(lib),'-landroid','-llog'],check=True)
    port=pipeline.Port(pipeline.Options(folder=a.out,native_keyboard=True)); port.aw=a.out/'build';port.logs=a.out/'logs';port.logs.mkdir(exist_ok=True);port.env=dict(os.environ)
    activity=[] if a.without_java else port.external_activity(); apk=a.out/'keyboard.apk'; package='org.nx2a.keyboardtest'
    subprocess.run([sys.executable,str(dm/'android/apk/mkapk.py'),str(lib),str(apk),str(a.out/'key.json'),
                    '--package',package,'--lib','dmhost',*map(str,activity)],check=True)
    adb=[str(a.sdk/'platform-tools/adb'),'-s',a.serial]
    def run(*args): return subprocess.check_output([*adb,*args],text=True,timeout=35)
    pid = []
    def logs(): return run('logcat','-d',*pid,'-s','nx2a-keyboard-test:I','*:S')
    def wait(fn,label):
        end=time.monotonic()+25
        while time.monotonic()<end:
            result=fn()
            if result: return result
            if 'FAIL' in logs(): raise AssertionError(logs())
            time.sleep(.2)
        raise AssertionError('timeout: '+label+'\n'+logs())
    def page(title):
        def read():
            run('shell','uiautomator','dump','/data/local/tmp/nx2a-keyboard.xml')
            text=run('shell','cat','/data/local/tmp/nx2a-keyboard.xml')
            nodes=list(ElementTree.fromstring(text).iter('node'))
            return nodes if any(n.get('text')==title for n in nodes) else None
        return wait(read,title)
    def button(nodes,name): return next(n for n in nodes if n.get('resource-id')=='android:id/'+name)
    def tap(n):
        x0,y0,x1,y1=map(int,re.findall(r'\d+',n.get('bounds')))
        run('shell','input','tap',str((x0+x1)//2),str((y0+y1)//2))
    run('install','-r',str(apk))
    activity_name = 'android.app.NativeActivity' if a.without_java else 'org.nx2a.GameActivity'
    try:
        run('shell','am','start','-W','-n',package+'/'+activity_name)
        pid[:] = ['--pid=' + run('shell','pidof',package).strip()]
        for title in (() if a.without_java else ('Unicode','UTF16','Minimum','Cancel','Background','Repeat','Bounded')):
            nodes=page(title)
            if title in ('Unicode','UTF16','Repeat'):
                field=next(n for n in nodes if n.get('class')=='android.widget.EditText')
                assert field.get('text')=='A🚀e\u0301Z', field.attrib
            if title=='Minimum':
                assert button(nodes,'button1').get('enabled')=='false'
                run('shell','input','text','ab'); nodes=page(title)
                assert button(nodes,'button1').get('enabled')=='true'
            if title=='Background':
                run('shell','input','keyevent','KEYCODE_HOME')
                wait(lambda:'PASS Background' in logs(),'background cancellation')
                run('shell','am','start','-W','-n',package+'/org.nx2a.GameActivity')
            else: tap(button(nodes,'button2' if title=='Cancel' else 'button1'))
        wait(lambda:'ALL PASSED' in logs(),'native results')
        (a.out/'results.log').write_text(logs()); print(logs())
    finally:
        run('shell','am','force-stop',package)
        run('uninstall',package)
        run('shell','rm','-f','/data/local/tmp/nx2a-keyboard.xml')


if __name__=='__main__': main()
