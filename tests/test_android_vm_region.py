"""The Android compat vm_region_64 (dm/android/compat/darwin_compat.c): tagged heap pointers and regions split over adjacent /proc/self/maps lines, cut from the source."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_cc   # noqa: E402

COMPAT = Path(__file__).resolve().parents[1] / 'nxnative/dm/android/compat'
HARNESS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach.h>
CUT
static int region(uint64_t a, uint64_t *at, uint64_t *sz, int *prot) {
    vm_address_t x = (vm_address_t)a; vm_size_t n = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    kern_return_t r = vm_region_64(0, &x, &n, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &obj);
    *at = x; *sz = n; *prot = bi.protection; return r == KERN_SUCCESS;
}
int main(void) {
    const uint64_t pg = (uint64_t)sysconf(_SC_PAGESIZE), TAG = 0xb4ull << 56;
    // five pages: anonymous, a private file mapping, anonymous (all rw-p: three maps lines, one region), read-only, then anonymous rw again
    uint8_t *b = mmap(NULL, 5 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); assert(b != MAP_FAILED);
    int fd = memfd_create("vm-region", 0); assert(fd >= 0); assert(!ftruncate(fd, (off_t)pg));
    assert(mmap(b + pg, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0) == b + pg);
    assert(!mprotect(b + 3 * pg, pg, PROT_READ));
    uint64_t at, sz; int prot;
    // an untagged pointer into the middle line: the whole run of three lines, not the read-only page after it
    assert(region((uint64_t)(uintptr_t)(b + pg + 8), &at, &sz, &prot));
    assert(at <= (uint64_t)(uintptr_t)b && at + sz == (uint64_t)(uintptr_t)(b + 3 * pg) && prot == (VM_PROT_READ | VM_PROT_WRITE));
    // the same with the top byte tagged: found, and given back with the tag, so the tagged range compares inside it
    uint64_t a = (uint64_t)(uintptr_t)(b + pg + 8) | TAG, n = 2 * pg - 16;
    assert(region(a, &at, &sz, &prot)); assert((at & ~0x00ffffffffffffffull) == TAG && at <= a && a + n <= at + sz);
    // the read-only page is a region of its own; the page after it starts another
    assert(region((uint64_t)(uintptr_t)(b + 3 * pg) | TAG, &at, &sz, &prot) && at == ((uint64_t)(uintptr_t)(b + 3 * pg) | TAG) && sz == pg && prot == VM_PROT_READ);
    assert(region((uint64_t)(uintptr_t)(b + 4 * pg), &at, &sz, &prot) && at == (uint64_t)(uintptr_t)(b + 4 * pg) && prot == (VM_PROT_READ | VM_PROT_WRITE));
    // a hole: the next region above it, as Mach answers
    assert(!munmap(b + 3 * pg, pg));
    assert(region((uint64_t)(uintptr_t)(b + 3 * pg), &at, &sz, &prot) && at == (uint64_t)(uintptr_t)(b + 4 * pg));
    // a malloc'd megabyte, tagged as Android tags it
    uint8_t *m = malloc(1 << 20); assert(m); a = (uint64_t)(uintptr_t)m | TAG;
    assert(region(a, &at, &sz, &prot) && at <= a && a + (1 << 20) <= at + sz && (prot & VM_PROT_WRITE));
    free(m); puts("vm_region ok"); return 0;
}
'''


@unittest.skipUnless(shutil.which('cc') and host_cc.posix(), 'a POSIX host C compiler is required')
@unittest.skipUnless(Path('/proc/self/maps').exists(), 'Linux /proc/self/maps is required')
class AndroidVmRegion(unittest.TestCase):
    def test_tagged_and_split(self):
        src = (COMPAT / 'darwin_compat.c').read_text(encoding='utf-8')
        cut = src[src.index('static int prot_of('):src.index('kern_return_t mach_vm_region(')]
        with tempfile.TemporaryDirectory(prefix='nx2a-vmregion-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(HARNESS.replace('CUT', cut), encoding='utf-8')
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(COMPAT / 'include'), str(c), '-o', str(exe)], check=True)
            out = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn('vm_region ok', out.stdout)


if __name__ == '__main__':
    unittest.main()
