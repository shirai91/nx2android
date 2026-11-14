"""Execute the production NVN capability query without an Android GPU or game assets."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class GpuCodeVersion(unittest.TestCase):
    def test_precompiled_shader_range_and_runtime_compiler_floor(self):
        source = (ROOT / 'nxnative/dm/gfx/nvn/hle_nvn.c').read_text(encoding='utf-8')
        table = source[source.index('static const int g_devinfo[]'):source.index('// The NVN API version')]
        funcs = source[source.index('static void nvn_api('):source.index('NVN(nvnDeviceGetImageHandle,')]
        sdk_define = next(line for line in (ROOT / 'nxnative/dm/host/dm.h').read_text(encoding='utf-8').splitlines() if line.startswith('#define DM_SDK_OLD('))
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
SDK_DEFINE
typedef void NVNdevice;
typedef unsigned NVNdeviceInfo;
#define NVN(name, ret, ...) ret name(__VA_ARGS__)
#define LOG(...) ((void)0)
#define OLOG(...) ((void)0)
static int sdk, compiler_major, compiler_minor;
static int dm_sdk_major(void) { return sdk; }
static int dm_glslc_gpu_code(int *major, int *minor) {
    *major = compiler_major; *minor = compiler_minor;
    return compiler_major > 0;
}
TABLE
FUNCTIONS
static int query(unsigned what) { int out = -1; nvnDeviceGetInteger(NULL, what, &out); return out; }
int main(int argc, char **argv) {
    assert(argc == 2); sdk = atoi(argv[1]);
    // Original Switch translators handle GPU code 1.16 even when the title links an older SDK.
    assert(query(0x2e) == 1 && query(0x30) == 16);
    // Astral Chain's precompiled hwfillcolor shader is 1.15 despite its SDK 6.4.
    assert(query(0x2e) > 1 || (query(0x2e) == 1 && query(0x30) >= 15));
    compiler_major = 1; compiler_minor = 15;
    assert(query(0x2e) == 1 && query(0x30) == 16);
    compiler_minor = 17;
    assert(query(0x2e) == 1 && query(0x30) == 17);
    compiler_major = 2; compiler_minor = 0;
    assert(query(0x2e) == 2 && query(0x30) == 0);
    assert(query(0) == (sdk <= 10 && sdk > 0 ? 53 : 55));
    assert(query(1) == (sdk <= 10 && sdk > 0 ? 313 : 15));
    assert(query(0x2f) == 1 && query(0x31) == 5);
    assert(query(2) == g_devinfo[2]);
    assert(query(0x67) == 256 && query(0xffff) == 0);
}
'''.replace('SDK_DEFINE', sdk_define).replace('TABLE', table).replace('FUNCTIONS', funcs)
        with tempfile.TemporaryDirectory(prefix='nx2a-nvn-code-') as tmp:
            c, exe = Path(tmp) / 'query.c', Path(tmp) / 'query'
            c.write_text(harness, encoding='utf-8')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT), str(c), '-o', str(exe)], check=True)
            for sdk in (6, 7, 8, 10, 11, 13, 20, 0):
                with self.subTest(sdk=sdk):
                    subprocess.run([str(exe), str(sdk)], check=True, timeout=10, env={k: v for k, v in os.environ.items() if k != 'DM_NVN_API'})


if __name__ == '__main__':
    unittest.main()
