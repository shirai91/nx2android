"""Sampler builders return their configured anisotropy and reduction mode."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import csrc

NVN = csrc.REPO / 'nxnative/dm/gfx/nvn'


class SamplerBuilderModes(unittest.TestCase):
    def test_setters_and_getters_round_trip(self):
        compiler = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
        if not compiler:
            self.skipTest('Native C compiler unavailable')
        source = (NVN / 'hle_nvn.c').read_text(encoding='utf-8')
        helpers = (
            '#define SB(b) ', 'NVN(nvnSamplerBuilderSetMaxAnisotropy,',
            'NVN(nvnSamplerBuilderSetReductionFilter,',
        )
        program = r'''
#include <assert.h>
#include "nvn_obj.h"
#include "nvn_protos.h"
#define NVN(name, ret, ...) ret h_##name(__VA_ARGS__)
'''
        for head in helpers:
            program += next(line for line in source.splitlines() if line.startswith(head)) + '\n'
        for name in ('SetDefaults', 'GetMaxAnisotropy', 'GetReductionFilter'):
            program += csrc.function(source, 'NVN(nvnSamplerBuilder' + name + ',')
        program += r'''
int main(void) {
    NVNT_nvnSamplerBuilderGetMaxAnisotropy anisotropy = h_nvnSamplerBuilderGetMaxAnisotropy;
    NVNT_nvnSamplerBuilderGetReductionFilter reduction = h_nvnSamplerBuilderGetReductionFilter;
    NVNsamplerBuilder builder = {0};
    h_nvnSamplerBuilderSetDefaults(&builder);
    assert(anisotropy(&builder) == 1.0f);
    assert(reduction(&builder) == NVN_SAMPLER_REDUCTION_AVERAGE);
    h_nvnSamplerBuilderSetMaxAnisotropy(&builder, 8.0f);
    h_nvnSamplerBuilderSetReductionFilter(&builder, NVN_SAMPLER_REDUCTION_MAX);
    assert(anisotropy(&builder) == 8.0f);
    assert(reduction(&builder) == NVN_SAMPLER_REDUCTION_MAX);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'check.c'
            executable = path.with_suffix('.exe')
            path.write_text(program, encoding='utf-8')
            subprocess.run([compiler, '-std=c11', '-O2', '-fno-strict-aliasing',
                            '-Werror=incompatible-pointer-types', '-I' + str(NVN),
                            '-I' + str(NVN / 'include'), str(path), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
