"""What the host C compiler can build, for the tests that compile C harnesses (not a test module).

The Windows release runner's `cc` is MinGW gcc: it has no <sys/mman.h>, no setenv and no
libubsan, so harnesses that mprotect pools or run under UBSan are skipped there; macOS and
Linux run them.
"""
import functools
from pathlib import Path
import shutil
import subprocess
import tempfile

POSIX_PROBE = r'''
#define _GNU_SOURCE
#include <sys/mman.h>
#include <pthread.h>
#include <stdlib.h>
int main(void) {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    pthread_mutex_lock(&mu); pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    pthread_cond_destroy(&cv); pthread_mutex_destroy(&mu);
    setenv("NX2A_PROBE", "1", 1); return mprotect(0, 0, PROT_READ) && 0;
}
'''


def _builds(code, *flags, compiler='cc'):
    if not shutil.which(compiler):
        return False
    with tempfile.TemporaryDirectory() as d:
        c = Path(d) / 'probe.c'
        c.write_text(code, encoding='utf-8')
        try:
            return subprocess.run([compiler, *flags, str(c), '-o', str(Path(d) / 'probe')], capture_output=True, timeout=60).returncode == 0
        except (OSError, subprocess.SubprocessError):
            return False


@functools.lru_cache(maxsize=None)
def posix():
    """cc builds a program with <sys/mman.h>, pthread mutexes/conditions and setenv."""
    return _builds(POSIX_PROBE, '-std=gnu11', '-pthread')


@functools.lru_cache(maxsize=None)
def ubsan():
    """cc links -fsanitize=undefined."""
    return _builds('int main(void) { return 0; }\n', '-std=c11', '-fsanitize=undefined', '-fno-sanitize-recover=all')


@functools.lru_cache(maxsize=None)
def asan_ubsan(compiler):
    """The selected compiler links the texture-key harness's ASan/UBSan flags."""
    return _builds('int main(void) { return 0; }\n', '-std=c11',
                   '-fsanitize=address,undefined', compiler=compiler)
