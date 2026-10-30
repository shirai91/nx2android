#!/usr/bin/env python3
# Android build of the host (the counterpart of dm/host/Makefile): the same host and graphics sources, the Darwin compatibility layer (dm/android/compat)
# and the Android front end / graphics backend (dm/android/*.c), built with the NDK's clang for arm64.
#   build.py --game <title ID> [--ndk <NDK dir>] [--out <dir>] [--jobs N] [--exe] [--lib] [--only-compile] [--release]
# --release: no debug information, the library stripped, DM_RELEASE defined (the app ignores DM_CTL), outputs in dm/android/out/<title ID>-release.
# Outputs in <out> (default dm/android/out/<title ID>): dmhost (an executable for adb shell runs: headless, no window) and libdmhost.so (the app's NativeActivity).
# Objects are rebuilt when a source or a header it includes changed (clang -MMD dependency files).
import argparse, concurrent.futures as cf, os, re, subprocess, sys
from pathlib import Path

DM = Path(__file__).resolve().parent.parent          # dm/
ap = argparse.ArgumentParser()
ap.add_argument('--game', required=True)
ap.add_argument('--ndk', default=os.environ.get('ANDROID_NDK_HOME') or str(DM.parent.parent / 'tools/android-ndk-r27c'))
ap.add_argument('--out')
ap.add_argument('--api', default='31')
ap.add_argument('--jobs', type=int, default=os.cpu_count() or 8)
ap.add_argument('--exe', action='store_true'); ap.add_argument('--lib', action='store_true')
ap.add_argument('--only-compile', action='store_true', help='compile every source and report errors, no link')
ap.add_argument('--keep-going', action='store_true')
ap.add_argument('-O', default='2')
ap.add_argument('--gfx', default='vk', choices=('vk', 'null'), help='graphics backend: Vulkan or the null device')
ap.add_argument('--release', action='store_true', help='a build to keep installed: no debug information, stripped, no DM_CTL')
a = ap.parse_args()
if not (a.exe or a.lib): a.exe = a.lib = True

host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64' if sys.platform.startswith('linux') else 'darwin-x86_64'
bin_ = Path(a.ndk) / 'toolchains/llvm/prebuilt' / host / 'bin'
CC = str(bin_ / ('clang.exe' if os.name == 'nt' else 'clang'))
TARGET = f'--target=aarch64-linux-android{a.api}'
out = Path(a.out) if a.out else DM / 'android/out' / (a.game + ('-release' if a.release else ''))
DBG = [] if a.release else ['-g']
obj = out / 'obj'; gen = out / 'gen'; obj.mkdir(parents=True, exist_ok=True); (gen / a.game).mkdir(parents=True, exist_ok=True)

# the game profile header, as the Makefile makes it
subprocess.run([sys.executable, str(DM / 'games/game.py'), 'header', a.game, str(gen / a.game / 'game_profile.h')], check=True)
# and its Android keys (android.tsv): the on-screen gamepad's layout
subprocess.run([sys.executable, str(DM / 'android/tools/android_profile.py'), 'header', a.game, str(gen / a.game / 'android_profile.h')], check=True)

# the backend's own small shaders (dm/android/shaders): compiled with the NDK's glslc, embedded as gen/<name>_<stage>.spv.h
GLSLC = Path(a.ndk) / 'shader-tools' / host / ('glslc.exe' if os.name == 'nt' else 'glslc')
for sh in sorted((DM / 'android/shaders').glob('*.*')):
    hdr = gen / a.game / f'{sh.stem}_{sh.suffix[1:]}.spv.h'
    if hdr.exists() and hdr.stat().st_mtime >= sh.stat().st_mtime: continue
    spv = gen / f'{sh.name}.spv'
    subprocess.run([str(GLSLC), '--target-env=vulkan1.1', '-O', str(sh), '-o', str(spv)], check=True)
    words = spv.read_bytes(); n = len(words) // 4
    import struct as _st
    body = ', '.join(f'{w:#010x}' for w in _st.unpack(f'<{n}I', words))
    hdr.write_text(f'// generated from dm/android/shaders/{sh.name} by build.py (glslc)\nstatic const uint32_t {sh.stem}_{sh.suffix[1:]}_spv[] = {{{body}}};\n')

def srcs(*globs): return [p for g in globs for p in sorted(DM.glob(g))]
HOST = [p for p in srcs('host/*.c') if p.name not in ('hle_movie_avc.c',)]   # VideoToolbox: dm/android/movie_avc.c instead
GFX = srcs('gfx/*.c', 'gfx/nvn/*.c', 'gfx/tex/*.c')
PORT = [p for p in srcs('android/compat/*.c', 'android/*.c') if not (p.name == 'gfx_null.c' and a.gfx != 'null') and not (p.name.startswith('gfx_vk') and a.gfx != 'vk')]   # one graphics backend
ASM = srcs('host/entry.S', 'gfx/nvn/nvn_entry.S', 'gfx/nvn/nvn_thunks.S')
VPX = srcs('third_party/libvpx/**/*.c')

INC = ['-include', str(DM / 'android/compat/include/darwin_compat.h'), f'-I{DM / "android/compat/include"}', f'-I{DM / "host"}', f'-I{DM / "gfx/nvn"}', f'-I{DM / "gfx/tex"}', f'-I{DM / "gfx/nvn/include"}',
       f'-I{DM / "third_party/vulkan-headers/include"}', f'-I{gen / a.game}', f'-I{DM / "third_party/libvpx"}', f'-I{DM / "third_party/stb_vorbis"}', f'-I{DM / "android"}']
CFLAGS = [TARGET, f'-O{a.O}'] + DBG + ['-fPIC', '-fno-omit-frame-pointer', '-ffixed-x18', '-Wall', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
          '-Wno-format', '-Wno-deprecated-declarations', '-Wno-gnu-folding-constant', '-Werror=implicit-function-declaration', '-Werror=return-type',
          '-Werror=incompatible-pointer-types', '-Werror=int-conversion', '-D_GNU_SOURCE', '-DDM_ANDROID=1', '-fno-strict-aliasing', '-fvisibility=hidden'] + (['-DDM_RELEASE=1'] if a.release else [])
VPX_FLAGS = [TARGET, '-O2'] + DBG + ['-fPIC', f'-I{DM / "third_party/libvpx"}', f'-I{DM / "third_party/libvpx/config"}', '-w']

import hashlib, shutil
_stamp = obj / 'flags.stamp'; _flags = hashlib.sha256(repr((CFLAGS, INC, VPX_FLAGS, CC)).encode()).hexdigest()
if not _stamp.exists() or _stamp.read_text() != _flags:   # compiler flags changed: every object is rebuilt
    shutil.rmtree(obj, ignore_errors=True); obj.mkdir(parents=True, exist_ok=True); _stamp.write_text(_flags)

def dep_stale(o, d, s):
    if not o.exists() or o.stat().st_mtime < s.stat().st_mtime: return True
    if not d.exists(): return True
    t = o.stat().st_mtime
    txt = d.read_text(errors='replace').replace('\\\n', ' ')
    for f in re.split(r'(?<!\\)\s+', txt.split(':', 1)[1] if ':' in txt else ''):
        f = f.replace('\\ ', ' ').strip()
        if f and Path(f).exists() and Path(f).stat().st_mtime > t: return True
    return False

def compile_one(s):
    rel = s.relative_to(DM); o = obj / (str(rel).replace(os.sep, '_').replace('/', '_') + '.o'); d = o.with_suffix('.d')
    src = s
    if s.suffix == '.S':   # Mach-O assembly → ELF
        src = gen / (s.stem + '.elf.S')
        if not src.exists() or src.stat().st_mtime < s.stat().st_mtime:
            subprocess.run([sys.executable, str(DM / 'android/tools/darwin2elf.py'), str(s), str(src)], check=True)
    if not dep_stale(o, d, src): return s, o, 0, ''
    flags = VPX_FLAGS if 'third_party/libvpx' in str(rel).replace(os.sep, '/') else [TARGET] + DBG if s.suffix == '.S' else CFLAGS + INC
    r = subprocess.run([CC] + flags + ['-MMD', '-MF', str(d), '-c', str(src), '-o', str(o)], capture_output=True, text=True, errors='replace')
    return s, o, r.returncode, r.stderr

def build(files):
    objs, failed = [], []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for s, o, rc, err in ex.map(compile_one, files):
            objs.append(o)
            if rc: failed.append(s); print(f'--- {s.relative_to(DM)}\n{err.strip()}\n', file=sys.stderr)
            elif err.strip() and '--verbose' in sys.argv: print(err, file=sys.stderr)
    return objs, failed

all_files = HOST + GFX + PORT + ASM
objs, failed = build(all_files)
vobjs, vfailed = build(VPX)
failed += vfailed
print(f'compiled {len(all_files) + len(VPX)} sources, {len(failed)} failed' + (': ' + ', '.join(str(f.relative_to(DM)) for f in failed) if failed else ''))
if failed and not a.keep_going: sys.exit(1)
if a.only_compile: sys.exit(1 if failed else 0)

LIBS = ['-landroid', '-llog', '-lvulkan', '-laaudio', '-lnativewindow', '-lmediandk', '-lz', '-lm', '-ldl']   # -lz: the gamepad theme's PNGs (overlay_theme.c)
LDFLAGS = [TARGET, '-fuse-ld=lld', '-Wl,--error-limit=0', '-Wl,-z,max-page-size=16384', '-Wl,--build-id', '-Wl,-z,nostart-stop-gc']
if a.exe:
    r = subprocess.run([CC] + LDFLAGS + ['-pie', '-o', str(out / 'dmhost')] + [str(o) for o in objs + vobjs] + LIBS, capture_output=True, text=True, errors='replace')
    print(r.stderr.strip() if r.returncode else f'linked {out / "dmhost"}', file=sys.stderr if r.returncode else sys.stdout)
    if r.returncode: sys.exit(1)
if a.lib:
    r = subprocess.run([CC] + LDFLAGS + ['-shared', '-Wl,-soname,libdmhost.so', '-o', str(out / 'libdmhost.so')] + [str(o) for o in objs + vobjs] + LIBS, capture_output=True, text=True, errors='replace')
    print(r.stderr.strip() if r.returncode else f'linked {out / "libdmhost.so"}', file=sys.stderr if r.returncode else sys.stdout)
    if r.returncode: sys.exit(1)
    if a.release:   # the static symbol table goes too (the dynamic one stays: ANativeActivity_onCreate)
        subprocess.run([str(bin_ / ('llvm-strip.exe' if os.name == 'nt' else 'llvm-strip')), '--strip-unneeded', str(out / 'libdmhost.so')], check=True)
        print(f'stripped {out / "libdmhost.so"}: {(out / "libdmhost.so").stat().st_size // 1024} KB')
