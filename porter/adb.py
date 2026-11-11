# adb for installing a port: the device list, installing the APK, and pushing the data pack in short transfers. Big pushes over one adb connection wedge adbd on some
# devices (a 6 GB RomFS), so small files go in batches (tar archives of up to 64 MB when there are many) and big files in 64 MB pieces that the device joins; a rerun resumes (files already at their size are skipped,
# pieces are kept if their checksums match). The port's own outputs are also compared by MD5: a rewriter cut changes a module without changing its size, and Unpacking's sdk cuts never
# reached the device until they were. Pushes use -Z: compressing them took adbd down.
import contextlib, hashlib, os, re, shlex, shutil, subprocess, tarfile, tempfile, time
from pathlib import Path


class AdbError(Exception):
    pass


NO_ADB = 'adb not found: install Android platform-tools (Download missing tools), or set its path with `porter config adb <path>`'


LONG_PATH = 240   # adb.exe is not long-path aware: it cannot open a file whose full path passes Windows' 260 characters (Viractal's Unity bundles in a deep game folder)
TAR_MIN, TAR_FILES, TAR_BYTES = 256, 8192, 64 << 20   # push_tree: from this many small files on, tar archives of at most this many files / bytes


def long_path(p):   # a path Python can open on Windows past 260 characters
    p = str(Path(p).resolve())
    return '\\\\?\\' + p if os.name == 'nt' and len(p) >= LONG_PATH and not p.startswith('\\\\') else p


@contextlib.contextmanager
def short_paths(paths):
    """The files under paths adb can open: as they are, or (on Windows, when one is too long) hard links with the same names in a short temporary folder, so they
    keep their names on the device"""
    if os.name != 'nt' or all(len(str(Path(p).resolve())) < LONG_PATH for p in paths):
        yield [str(p) for p in paths]; return
    with tempfile.TemporaryDirectory(prefix='nx2a_') as t:
        out = []
        for p in paths:
            q = Path(t) / Path(p).name
            try: os.link(p, q)
            except OSError: shutil.copy2(p, q)
            out.append(str(q))
        yield out


class Adb:
    def __init__(self, exe, serial=None, timeout=120, log=print):
        if not exe: raise AdbError(NO_ADB)   # str(None) would run a program named "None" (issue #109: FileNotFoundError out of the bundle)
        self.exe, self.serial, self.timeout, self.log = str(exe), serial or None, timeout, log
        self.env = dict(os.environ, MSYS_NO_PATHCONV='1')   # Git Bash would rewrite /sdcard/... arguments into Windows paths

    def cmd(self, *a):
        return [self.exe] + (['-s', self.serial] if self.serial else []) + [str(x) for x in a]

    def run(self, *a, timeout=None, check=True, retries=3):
        err = ''
        for attempt in range(retries):
            try:
                r = subprocess.run(self.cmd(*a), capture_output=True, text=True, errors='replace', timeout=timeout or self.timeout, env=self.env)
                if r.returncode == 0 or not check: return r.stdout
                err = (r.stderr.strip() or r.stdout.strip())[-400:]   # adb's reason is on stderr; stdout has the files that went
                if a and a[0] == 'install': break   # an install failure is an answer, not a dropped link
            except subprocess.TimeoutExpired: err = f'timed out after {timeout or self.timeout} s'
            except OSError as e: raise AdbError(f'cannot run adb ({self.exe}): {e}') from e   # adb removed or moved: no retry brings it back
            if attempt + 1 < retries:
                self.log(f'adb {a[0]} failed ({err}), retry {attempt + 1}')
                try: subprocess.run(self.cmd('wait-for-device'), timeout=300, env=self.env)
                except subprocess.TimeoutExpired: raise AdbError(f'adb {a[0]} failed ({err}) and the device did not come back within 300 s') from None
                time.sleep(2)
        raise AdbError(f'adb {a[0]}: {err}')

    def shell(self, script, **kw): return self.run('shell', script, **kw)

    # ---- devices ----
    def devices(self):
        """[(serial, state, description)] of every device adb sees"""
        try: out = subprocess.run([self.exe, 'devices', '-l'], capture_output=True, text=True, errors='replace', timeout=30, env=self.env).stdout
        except OSError as e: raise AdbError(f'cannot run adb ({self.exe}): {e}') from e
        except subprocess.TimeoutExpired: raise AdbError('adb devices: timed out after 30 s') from None
        rows = []
        for ln in out.splitlines()[1:]:
            parts = ln.split()
            if len(parts) >= 2: rows.append((parts[0], parts[1], ' '.join(p for p in parts[2:] if p.startswith(('model:', 'device:')))))
        return rows

    def pick(self):   # the configured device, else the only one connected
        ready = [s for s, st, _ in self.devices() if st == 'device']
        if self.serial:
            if self.serial not in ready: raise AdbError(f'device {self.serial} is not connected (connected: {", ".join(ready) or "none"})')
            return self.serial
        if len(ready) != 1: raise AdbError('no device connected' if not ready else f'several devices connected ({", ".join(ready)}): choose one')
        self.serial = ready[0]
        return self.serial

    def prop(self, name): return self.shell(f'getprop {shlex.quote(name)}').strip()

    def gpu(self):   # the GPU's name as SurfaceFlinger reports it ("Qualcomm, Adreno (TM) 830, OpenGL ES 3.2 ..."), '' when unknown
        out = self.shell('dumpsys SurfaceFlinger 2>/dev/null | grep -m1 "^GLES:" ; true', check=False).strip()
        return out[len('GLES:'):].strip() if out.startswith('GLES:') else ''

    def bc_textures(self):
        """Whether the GPU samples BC-compressed textures (the Switch's format, profile key requires bcn): Adreno and Xclipse do, Mali and PowerVR do not; None when unknown"""
        g = self.gpu().lower()
        if any(v in g for v in ('adreno', 'xclipse', 'nvidia')): return True
        if any(v in g for v in ('mali', 'powervr', 'immortalis')): return False
        return None

    def free_bytes(self, path='/sdcard'):   # free bytes on path's volume (its nearest folder that exists: a folder still to be made counts as its parent's), None when unknown
        out = self.shell(f'p={shlex.quote(path)}; while [ ! -e "$p" ]; do p=$(dirname "$p"); done; df -k "$p" | tail -1', check=False).split()
        return int(out[3]) * 1024 if len(out) >= 4 and out[3].isdigit() else None

    def same_volume(self, a, b):   # whether two paths (their nearest folders that exist) share a volume and its free space: stat's device number (an SD card is a volume of its own)
        # -L: /sdcard is a symlink, and toybox stat does not follow one without it (the root file system's device number, never shared storage's)
        out = self.shell('for p in ' + ' '.join(shlex.quote(x) for x in (a, b)) + '; do while [ ! -e "$p" ]; do p=$(dirname "$p"); done; stat -L -c %d "$p"; done', check=False).split()
        return len(out) == 2 and out[0] == out[1]

    def read_text(self, path):   # a small text file on the device, '' when it is not there
        return self.shell(f'cat {shlex.quote(path)} 2>/dev/null; true', check=False)

    def sdk(self):   # the device's API level (29: Android 10), 0 when unknown
        v = self.prop('ro.build.version.sdk')
        return int(v) if v.isdigit() else 0

    def requests_permission(self, package, perm):   # whether the installed app's manifest asks for perm (dumpsys lists only the permissions an app asks for)
        return perm in self.shell(f'dumpsys package {shlex.quote(package)} 2>/dev/null | grep -F {shlex.quote(perm)}; true', check=False)

    def storage_access(self, package, grant=False):
        """Whether the app may read shared storage outside its own folders (a RomFS read from a game dump's folder: install --romfs-at): All files access, the appop
        MANAGE_EXTERNAL_STORAGE, from Android 11 (the switch Settings shows sets the app's uid mode, as `appops set --uid` does); on Android 10, which has no All files access,
        READ_EXTERNAL_STORAGE (the APK asks for the legacy storage view there: mkapk.py). grant: have the shell grant it first, which adb may do without the player.
        Returns (granted, what the device says)"""
        q = shlex.quote(package)
        if self.sdk() >= 30:
            def mode():   # "Uid mode: MANAGE_EXTERNAL_STORAGE: allow"; "No operations." while it was never set (the default: not granted)
                m = re.search(r'MANAGE_EXTERNAL_STORAGE: (\w+)', self.shell(f'appops get --uid {q} MANAGE_EXTERNAL_STORAGE 2>&1; true', check=False))
                return m.group(1) if m else 'default'
            if grant and mode() != 'allow': self.shell(f'appops set --uid {q} MANAGE_EXTERNAL_STORAGE allow 2>&1; true', check=False)
            got = mode()
            return got == 'allow', f'All files access (appop MANAGE_EXTERNAL_STORAGE): {got}'
        perm = 'android.permission.READ_EXTERNAL_STORAGE'
        if grant: self.shell(f'pm grant {q} {perm} 2>&1; true', check=False)
        ok = f'{perm}: granted=true' in self.shell(f'dumpsys package {q} 2>/dev/null | grep -F {perm}: ; true', check=False)
        return ok, f'{perm}: {"granted" if ok else "not granted"}'

    def installed(self, package):
        return f'package:{package}' in self.shell(f'pm list packages {shlex.quote(package)}', check=False).split()

    def version_code(self, package):   # the installed app's versionCode, None when unknown
        try: out = self.shell(f'dumpsys package {shlex.quote(package)} | grep -m1 versionCode=', check=False)
        except AdbError: return None
        m = re.search(r'versionCode=(\d+)', out)
        return int(m.group(1)) if m else None

    # ---- install ----
    def install(self, apk, package, replace_signer=False):
        """adb install -r. A package installed with another signing key cannot be updated: replace_signer uninstalls it first (its saves in internal storage go with it)"""
        try: self.run('install', '-r', apk, timeout=600, retries=1)
        except AdbError as e:
            if 'INSTALL_FAILED_VERSION_DOWNGRADE' in str(e):   # Android installs over the same versionCode, never over a higher one
                have = self.version_code(package)
                raise AdbError(f'{package} is installed with a higher versionCode{f" ({have})" if have else ""} (a port of a later game version, a rebuild with --vercode-offset): '
                               'port again with a --vercode-offset that lifts this build above it (the saves stay), install it beside that one with --package, '
                               'or uninstall that one on the device (its saves are deleted with it)') from e
            if 'INSTALL_FAILED_UPDATE_INCOMPATIBLE' not in str(e): raise
            if not replace_signer:
                raise AdbError(f'{package} is installed with another signing key (another build of the game, a development build): '
                               'uninstall it on the device, or install with --replace-signer (its saves are deleted with it)') from e
            self.log(f'{package} has another signing key: uninstalling it first')
            self.run('uninstall', package, timeout=300, retries=1)
            self.run('install', apk, timeout=600, retries=1)

    def start_app(self, package):
        """Start the installed package's launcher, including optional Java activities and older ports."""
        out = self.shell(f'cmd package resolve-activity --brief -a android.intent.action.MAIN -c android.intent.category.LAUNCHER {shlex.quote(package)}')
        components = [ln.strip() for ln in out.splitlines() if re.fullmatch(re.escape(package) + r'/[A-Za-z_.$][A-Za-z0-9_.$]*', ln.strip())]
        if len(components) != 1:
            raise AdbError(f'{package}: cannot resolve its launcher activity: {out.strip()}')
        out = self.shell(f'am start -W -n {shlex.quote(components[0])} 2>&1', timeout=60)
        if re.search(r'(?im)^(?:Error|Exception)', out):
            raise AdbError(f'{package}: could not start its launcher activity: {out.strip()}')

    def hold(self, folder):
        """One porter install per device at a time (two at once on one device uninstalled each other's app, and Android deleted the data and the saves with it): a lock
        file per serial in folder, held until the returned file is closed; AdbError when another install has it. No lock where the system has no flock (Windows)"""
        try: import fcntl
        except ImportError: return None
        f = open(Path(folder) / ('install-' + re.sub(r'[^A-Za-z0-9._-]', '_', self.serial or 'device') + '.lock'), 'w')
        try: fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as e: f.close(); raise AdbError(f'another porter install is using {self.serial or "the device"}: wait for it to end') from e
        return f

    def prepare_app_files(self, package, timeout=30):
        """Have Android create the app's files folder (/sdcard/Android/data/<package>/files) before anything is pushed into it: the system makes it, owned by the app with
        the app's group, when the app first asks for it. A folder the shell makes first lacks that group underneath FUSE, and the app cannot read what is pushed below it
        (NEO's port died at its first open, mods/main.fixups.tsv). So the app is started once (it stops at once without its data) and stopped again.
        A folder already there that the app did not make (pushed by hand before the app first ran, or left by an install with another uid) is moved aside, made again by the
        app, and its contents moved back in (a rename: nothing is pushed again). Issue #9: on a Honor tablet the app opened its modules by name but could not list RomFS, and
        Mario vs. Donkey Kong crashed at boot"""
        files = f'/sdcard/Android/data/{package}/files'; old = f'{files}.nx2a-old'
        isdir = lambda p: self.shell(f'[ -d {shlex.quote(p)} ] && echo yes; true').strip() == 'yes'
        if isdir(files):
            uid, owner = self.app_uid(package), self.shell(f'stat -c %u {shlex.quote(files)}', check=False).strip()
            if uid is None or owner == str(uid):
                if isdir(old): self.move_back(old, files)   # an earlier repair stopped before its contents went back
                return False
            if isdir(old): raise AdbError(f'{files} needs to be made again by the app, but {old} is already there (left by an earlier repair): move what you need out of it and delete it')
            self.log(f'{files} belongs to uid {owner}, not to the app (uid {uid}): letting the app make it again and moving its contents back in')
            self.shell(f'mv {shlex.quote(files)} {shlex.quote(old)}')
        self.start_app(package)
        for _ in range(timeout * 2):
            if isdir(files): break
            time.sleep(0.5)
        else: raise AdbError(f'{package} did not create its files folder ({files}) within {timeout} s of starting' + (f'; its old contents are in {old}' if isdir(old) else ''))
        self.shell(f'am force-stop {package}')
        if isdir(old): self.move_back(old, files)
        return True

    def app_uid(self, package):   # the app's Linux uid for the device's user 0, None when the package is not installed
        prev = ''
        for ln in self.shell(f'pm list packages -U {shlex.quote(package)}', check=False).split():
            if ln.startswith('uid:') and prev == f'package:{package}': return int(ln[4:].split(',')[0])
            prev = ln
        return None

    def move_back(self, old, files):   # everything in old into files (a rename on the same storage: nothing is copied), then old removed
        q, f = shlex.quote(old), shlex.quote(files)
        self.shell(f'cd {q} && for e in * .[!.]*; do [ -e "$e" ] && mv -f "$e" {f}/; done; cd / && rmdir {q}')

    def pull_new(self, rdir, local, pattern=None):   # the files directly in rdir that local lacks (at their size), pulled into local → their names
        local = Path(local); got = []
        for rel, sz in sorted(self.remote_sizes(rdir).items()):
            if '/' in rel or (pattern and not pattern(rel)) or ((local / rel).is_file() and (local / rel).stat().st_size == sz): continue
            local.mkdir(parents=True, exist_ok=True); self.run('pull', f'{rdir}/{rel}', local / rel); got.append(rel)
        return got

    def remote_sizes(self, rdir):
        """{relative path: size} of every file under rdir on the device ({} when rdir is not there). find's own -printf, not -exec stat {} +: toybox 0.8.6 (AYN Thor,
        Android 13) fails most -exec batches with "Argument list too long", and the listing came back short (6,759 of 206,904 files), so an install pushed everything again.
        Android 10's toybox has no -printf (11's has): there -exec stat stays. A find that fails is an AdbError, never a short listing"""
        q = shlex.quote(rdir)
        out = self.shell(f"[ -d {q} ] || exit 0; cd {q} && if find /dev/null -printf '' 2>/dev/null; then find . -type f -printf '%s %P\\n' 2>&1; "
                         f'else find . -type f -exec stat -c "%s %n" {{}} + 2>&1; fi; echo "rc=$?"')
        lines = out.splitlines()
        if lines and lines[-1].strip() != 'rc=0':
            bad = [ln for ln in lines[:-1] if not ln.partition(' ')[0].isdigit()]
            raise AdbError(f'listing {rdir} on the device failed ({lines[-1].strip() if lines[-1].startswith("rc=") else "no exit status"}): ' + ('; '.join(bad[-3:]) or 'no message'))
        have = {}
        for ln in lines[:-1]:
            sz, _, name = ln.partition(' ')
            if sz.isdigit(): have[name[2:] if name.startswith('./') else name] = int(sz)
        return have

    def remote_md5(self, rdir, rels, **kw):   # {relative path: MD5} of those files under rdir (missing ones left out)
        out, q = {}, shlex.quote(rdir)
        for i in range(0, len(rels), 64):
            for ln in self.shell(f'cd {q} && md5sum ' + ' '.join(shlex.quote(r) for r in rels[i:i + 64]) + ' 2>/dev/null; true', **kw).splitlines():
                h, _, name = ln.partition('  ')
                if len(h) == 32: out[name] = h
        return out

    def push_tree(self, local, remote, chunk=64 << 20, skip=(), keep=(), verify=None, progress=None, have=None, exclude=()):
        """local folder → remote folder. skip: relative paths not pushed; exclude: top-level folders left out whole (the RomFS, when the app reads it from another
        folder: install --romfs-at); keep: relative paths pushed only when the device has none (the player's settings);
        verify(rel): files whose content can change at the same size (the rewritten modules: a rewriter cut keeps the module's size), pushed again when their MD5 differs.
        progress(done_bytes, total_bytes, note) after each transfer. have: remote_sizes(remote) when the caller listed it already (a big pack takes seconds to list).
        Returns the bytes pushed"""
        local, remote = Path(local), remote.rstrip('/')
        files = sorted((p.relative_to(local).as_posix(), p) for top in local.iterdir() if top.name not in exclude for p in (top.rglob('*') if top.is_dir() else [top]) if p.is_file())
        if have is None: have = self.remote_sizes(remote)
        same = [rel for rel, p in files if verify and verify(rel) and rel not in keep and rel not in skip and have.get(rel) == p.stat().st_size]
        rmd5 = self.remote_md5(remote, same) if same else {}
        changed = {rel for rel, p in files if rel in same and rmd5.get(rel) != hashlib.md5(p.read_bytes()).hexdigest()}
        todo = [(rel, p, p.stat().st_size) for rel, p in files
                if rel not in skip and not rel.startswith('.dmpart') and (rel not in have if rel in keep else have.get(rel) != p.stat().st_size or rel in changed)]
        total, done = sum(s for _, _, s in todo), 0
        self.log(f'{len(todo):,} files to push, {total / 2**20:,.0f} MB ({len(files) - len(todo):,} already on the device)')
        dirs = [shlex.quote(f'{remote}/{d}' if d else remote) for d in sorted({os.path.dirname(r) for r, _, _ in todo})]
        for i in range(0, len(dirs), 200): self.shell('mkdir -p ' + ' '.join(dirs[i:i + 200]))   # one shell per directory took 3 min for Tears of the Kingdom's 2,669
        small = [(r, p, s) for r, p, s in todo if s <= chunk]
        if len(small) >= TAR_MIN:   # many small files: tar archives unpacked on the device
            done = self.push_tars(local, remote, small, progress=progress and (lambda d, note: progress(d, total, note)))
        else:
            for i in range(0, len(small), 32):   # small files: up to 32 per push, one push per directory
                part, by = small[i:i + 32], {}
                for r, p, s in part: by.setdefault(os.path.dirname(r), []).append(p)
                for d, ps in by.items():
                    with short_paths(ps) as qs:
                        self.run('push', '-Z', *qs, f'{remote}/{d}/' if d else f'{remote}/', timeout=max(self.timeout, 600))
                done += sum(s for _, _, s in part)
                if progress: progress(done, total, f'{len(part)} files')
        for rel, p, size in [(r, p, s) for r, p, s in todo if s > chunk]:
            base = done
            self.push_file(p, f'{remote}/{rel}', chunk, progress=progress and (lambda d, t, note: progress(base + d, total, note)))
            done += size
        return total

    def push_tars(self, local, remote, small, progress=None):
        """small [(relative path, local path, size)] → remote, as tar archives of at most TAR_BYTES / TAR_FILES built beside local, pushed in one transfer each and
        unpacked by the device's tar. 32 files per adb push cost ~2 s a push, ~1 MB/s for Tears of the Kingdom's 304,000 files of ~57 kB (hours); an archive goes at the
        link's speed. An archive cut short by a dropped link is not unpacked; a rerun pushes what is missing. progress(done_bytes, note) after each archive. Returns the bytes"""
        done, q = 0, shlex.quote(remote)
        with tempfile.TemporaryDirectory(prefix='.nx2a_tar_', dir=Path(local).parent) as t:   # beside the data: same drive, deleted with it
            arc = Path(t) / 'p.tar'
            i = 0
            while i < len(small):
                part, size = [], 0
                while i < len(small) and len(part) < TAR_FILES and (not part or size + small[i][2] <= TAR_BYTES): part.append(small[i]); size += small[i][2]; i += 1
                with tarfile.open(arc, 'w', format=tarfile.GNU_FORMAT) as tar:   # GNU long names: toybox reads them
                    for rel, p, s in part:
                        ti = tarfile.TarInfo(rel); ti.size, ti.mode, ti.mtime = s, 0o644, int(time.time())
                        with open(long_path(p), 'rb') as f: tar.addfile(ti, f)
                with short_paths([arc]) as (qa,):
                    self.run('push', '-Z', qa, f'{remote}/.dmpart.tar', timeout=max(self.timeout, 600))
                out = self.shell(f'cd {q} && tar -xf .dmpart.tar 2>&1; rc=$?; rm -f .dmpart.tar; echo "rc=$rc"', timeout=max(self.timeout, 600))
                if not out.rstrip().endswith('rc=0'): raise AdbError(f'unpacking an archive of {len(part)} files in {remote} failed: {out.strip()[-300:]}')
                done += size
                if progress: progress(done, f'{len(part):,} files')
        return done

    def push_file(self, local, dst, chunk=64 << 20, progress=None):
        """One file in pieces of chunk bytes, joined on the device; matching pieces are kept after checking their MD5, so an interrupted push resumes. progress(done, size, note)"""
        local = Path(local); size = local.stat().st_size; n = max(1, (size + chunk - 1) // chunk)
        pdir = f'{os.path.dirname(dst)}/.dmpart-{hashlib.sha1(dst.encode()).hexdigest()[:12]}'   # one piece folder per file: a resumed push finds its own pieces
        self.shell(f'mkdir -p {shlex.quote(pdir)}'); pieces = self.remote_sizes(pdir)
        same = [f'{k:05d}' for k in range(n) if pieces.get(f'{k:05d}') == min(chunk, size - k * chunk)]
        md5 = {}
        batch = max(1, min(8, (256 << 20) // chunk))
        for i in range(0, len(same), batch):
            try:
                md5.update(self.remote_md5(pdir, same[i:i + batch], timeout=min(self.timeout, 120), retries=1))
            except AdbError as e:
                self.log(f'{local.name}: could not verify retained pieces ({e}); sending the unverified pieces again')
                break   # a slow device should not pay another checksum timeout for every remaining batch
        with tempfile.TemporaryDirectory(prefix='nx2a_push_') as t, open(local, 'rb') as f:
            for k in range(n):
                want, name = min(chunk, size - k * chunk), f'{k:05d}'
                f.seek(k * chunk); data = f.read(want)
                if len(data) != want: raise AdbError(f'{local.name}: source changed during the push (short read); try again')
                if pieces.get(name) != want or md5.get(name) != hashlib.md5(data).hexdigest():
                    tmp = Path(t) / 'piece.bin'
                    tmp.write_bytes(data)
                    self.run('push', '-Z', tmp, f'{pdir}/{name}', timeout=max(self.timeout, 600))
                del data
                if progress: progress(k * chunk + want, size, f'{local.name} piece {k + 1}/{n}')
            if os.fstat(f.fileno()).st_size != size: raise AdbError(f'{local.name}: source changed size during the push; try again')
        q, qp = shlex.quote(dst), shlex.quote(pdir)   # join the pieces in order, then check the size
        # Join only this source's pieces: a shorter file or a different chunk size may leave old tail pieces in the same folder.
        for i in range(0, n, 256):   # keep each command short even when there are many pieces
            names = ' '.join(f'{k:05d}' for k in range(i, min(i + 256, n)))
            start = f'rm -f {q} && ' if i == 0 else ''
            # Retrying an interrupted append could duplicate data; a later push starts the join over.
            self.shell(f'{start}(cd {qp} && cat {names}) >> {q}', timeout=1200, retries=1)
        got = self.shell(f'stat -c %s {q}').strip()
        if got != str(size): raise AdbError(f'{local.name}: {got} bytes on the device, expected {size}')
        self.shell(f'rm -rf {qp}')
