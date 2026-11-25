"""The NVN API version for SDK 10 and older: nvn_api takes 53 and the minor read from the sdk's own nvnDeviceGetInteger switch (loader.c dm_sdk_nvn1_minor),
53.313 when that switch is not recognized; SDK 11 and later keep 55.15."""
from pathlib import Path
import subprocess, tempfile, unittest
import csrc
import host_cc

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


LOADER_PRELUDE = r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef struct { char name[16]; uint8_t *base; uint64_t text_end, ro_off, data_off; } module_t;
static module_t g_mod[2]; static int g_nmod;
'''


@unittest.skipUnless(host_cc.posix(), 'a POSIX host C compiler is required')
class NvnApiMinor(unittest.TestCase):
    def test_old_sdk_minor_from_its_switch(self):
        # nvn_api: SDK 10 and older take 53 and the sdk's own minor, 313 when it cannot be read; SDK 11 and later 55.15
        out = run(r'''
int main(void) {
 int api[2];
 sdk_major = 5; nvn1_minor = 209; nvn_api(api); assert(api[0] == 53 && api[1] == 209);
 nvn1_minor = -1; nvn_api(api); assert(api[0] == 53 && api[1] == 313);
 sdk_major = 12; nvn1_minor = 209; nvn_api(api); assert(api[0] == 55 && api[1] == 15);
 puts("api: PASS"); return 0;
}
''')
        self.assertIn('PASS', out, out)

    def test_switch_read_from_sdk_text(self):
        # dm_sdk_nvn1_minor on a synthetic sdk laid out as SDK 5.4.0's switch: adrp/add x9, table; ldrsw; add; mov w0, #53; br x8, a word table in
        # read-only data, case 0 the epilogue, case 1 mov w0, #<minor>; b <epilogue>
        fn = csrc.function(csrc.read('nxnative/dm/host/loader.c'), 'int dm_sdk_nvn1_minor(void) {', 'loader.c dm_sdk_nvn1_minor')
        main = r'''
static uint8_t img[0x3000] __attribute__((aligned(4096)));
static void put(uint64_t off, uint32_t w) { memcpy(img + off, &w, 4); }
static void build(uint32_t minor, int second) {
 memset(img, 0, sizeof img);
 for (int k = 0; k <= second; k++) {
  uint64_t p = 0x100 + 0x400 * k, table = 0x1000 + 0x10 * k, major_pc = p + 0x100, minor_pc = p + 0x120;
  int64_t page = (int64_t)(table & ~0xfffull) / 4096 - (int64_t)(p & ~0xfffull) / 4096;
  put(p, 0x90000009u | (uint32_t)(page & 3) << 29 | (uint32_t)((page >> 2) & 0x7ffff) << 5);
  put(p + 4, 0x91000129u | (uint32_t)(table & 0xfff) << 10); put(p + 8, 0xb8a87928); put(p + 12, 0x8b090108); put(p + 16, 0x528006a0); put(p + 20, 0xd61f0100);
  int32_t offs[2] = {(int32_t)(major_pc - table), (int32_t)(minor_pc - table)}; memcpy(img + table, offs, 8);
  put(major_pc, 0xa9427bfd); put(major_pc + 4, 0xa9414ff4); put(major_pc + 8, 0x9100c3ff); put(major_pc + 12, 0xd65f03c0);
  put(minor_pc, 0x52800000u | minor << 5); put(minor_pc + 4, 0x14000000u | ((uint32_t)(((int64_t)major_pc - (int64_t)(minor_pc + 4)) / 4) & 0x3ffffff));
 }
}
int main(void) {
 g_nmod = 2; strcpy(g_mod[0].name, "main"); strcpy(g_mod[1].name, "sdk");
 g_mod[1] = (module_t){"sdk", img, 0x1000, 0x1000, 0x2000};
 build(209, 0); assert(dm_sdk_nvn1_minor() == 209);
 build(209, 0); put(0x100 + 16, 0x52800020); assert(dm_sdk_nvn1_minor() == -1);   /* not mov w0, #53: another switch */
 build(209, 1); assert(dm_sdk_nvn1_minor() == -1);                                   /* two matches: not trusted */
 build(209, 0); g_mod[1].name[0] = 'x'; assert(dm_sdk_nvn1_minor() == -1);           /* no sdk module */
 puts("switch: PASS"); return 0;
}
'''
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / 'sw.c'; exe = Path(td) / 'sw'; src.write_text(LOADER_PRELUDE + fn + main, encoding='utf-8')
            r = subprocess.run(['cc', '-std=gnu11', '-O1', str(src), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertIn('PASS', out.stdout, out.stdout + out.stderr)


if __name__ == '__main__': unittest.main()
