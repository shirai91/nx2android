"""Hard links through root, for Termux: Python on Android has no os.link, and the app's own SELinux domain denies link() (`ln` as the app: Permission denied), while su's context may link (same inode, same owner).
Used only where os.link is missing and NX2A_SU_LINK is set; any failure returns False and the caller copies as before."""
import os, shlex, subprocess

def enabled(): return not hasattr(os, 'link') and bool(os.environ.get('NX2A_SU_LINK'))

def su_link(src, dst):
    try: return subprocess.run(['su', '-c', f'ln {shlex.quote(str(src))} {shlex.quote(str(dst))}'], capture_output=True, timeout=60).returncode == 0
    except (OSError, subprocess.SubprocessError): return False
