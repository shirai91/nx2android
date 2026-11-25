"""nvnDeviceGetInteger: items 0x5F to 0x68 answer the Switch driver's own constants, and items past them answer 0 with a red line."""
from pathlib import Path
import shutil, subprocess, tempfile, unittest
import csrc

NVN = csrc.REPO / 'nxnative/dm/gfx/nvn'


def nvn_function(source, name):
    start = source.index('NVN(' + name + ','); end = source.index('{', start) + 1; depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[start:end]


def device_source():
    source = csrc.read('nxnative/dm/gfx/nvn/hle_nvn.c')
    table = csrc.between(source, 'static const int g_devinfo[] = {', '\n};\n', 'hle_nvn.c g_devinfo') + '\n};\n'
    api = csrc.function(source, 'static void nvn_api(int api[2]) {', 'hle_nvn.c nvn_api')
    return table + api + nvn_function(source, 'nvnDeviceGetInteger')


PRELUDE = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "nvn_protos.h"
static int reds;
#include <string.h>
static void log_line(const char *fmt, ...) { reds += !strncmp(fmt, "[RED]", 5); }
#define LOG(...) log_line(__VA_ARGS__)
#define OLOG(...) ((void)0)
#define DM_SDK_OLD(major) ((major) > 0 && (major) <= 10)
static int sdk_major, nvn1_minor = -1;
static int dm_sdk_major(void) { return sdk_major; }
static int dm_sdk_nvn1_minor(void) { return nvn1_minor; }
static int dm_glslc_gpu_code(int *major, int *minor) { *major = *minor = 0; return 0; }
#define NVN(name,ret,...) static ret h_##name(__VA_ARGS__); static NVNT_##name entry_##name=h_##name; static ret h_##name(__VA_ARGS__)
'''


def run(main):
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / 'dev.c'; exe = Path(td) / 'dev'; src.write_text(PRELUDE + device_source() + main, encoding='utf-8')
        r = subprocess.run(['cc', '-std=gnu11', '-O1', '-Wno-unused-function', '-I' + str(NVN), '-I' + str(NVN / 'include'), str(src), '-o', str(exe)],
                           capture_output=True, text=True)
        if r.returncode: raise AssertionError(r.stderr)
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        return out.stdout + out.stderr


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class NvnDeviceInfo(unittest.TestCase):
    def test_driver_constants_past_the_measured_table(self):
        out = run(r'''
static int item(unsigned w) { int v = -7; entry_nvnDeviceGetInteger(NULL, (NVNdeviceInfo)w, &v); return v; }
int main(void) {
 const int want[] = {32, 65535, 1, 32, 256, 16, 65535, 65535, 256, 16383};   /* 0x5F .. 0x68 */
 for (unsigned i = 0; i < 10; i++) if (item(0x5F + i) != want[i]) { printf("item %#x = %d, want %d\n", 0x5F + i, item(0x5F + i), want[i]); return 1; }
 assert(item(0x5E) == 128 && reds == 0);
 assert(item(0x69) == 0 && reds == 1);
 puts("device info: PASS"); return 0;
}
''')
        self.assertIn('PASS', out, out)


if __name__ == '__main__': unittest.main()
