"""Ported alpha-test SPIR-V semantics and command/layout integration; no game assets."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'
sys.path.insert(0, str(ROOT))
from porter.toolchain import find_ndk


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'


class AlphaShaders(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        ndk, _ = find_ndk()
        cls.glslc = next(ndk.glob('shader-tools/*/glslc'), None) if ndk else None
        cls.val = next(ndk.glob('shader-tools/*/spirv-val'), None) if ndk else None
        if not cls.glslc or not cls.val or not shutil.which('cc'):
            raise unittest.SkipTest('NDK shader tools and host C compiler required')
        cls.tmp = tempfile.TemporaryDirectory(prefix='vk-alpha-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.d = Path(cls.tmp.name)
        cls.exe = cls.d / 'alpha'
        c = cls.d / 'alpha.c'
        c.write_text(r'''
#include "spv_alpha.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 3) return 1;
    FILE *f=fopen(argv[1],"rb"); if (!f) return 1;
    fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
    uint32_t *src=malloc((size_t)n); if (!src) return 1;
    if (fread(src,1,(size_t)n,f)!=(size_t)n) return 1; fclose(f);
    size_t words=0; const char *why=NULL;
    uint32_t *out=spv_alpha(src,(size_t)n/4,&words,&why); free(src);
    if (!out) { fprintf(stderr,"%s\n",why); return 2; }
    f=fopen(argv[2],"wb"); if (!f) return 1;
    int ok=fwrite(out,4,words,f)==words && !fclose(f); free(out); return !ok;
}
''')
        subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-I' + str(DM / 'android'), str(c), '-o', str(cls.exe)], check=True)

    def compile(self, source):
        f = self.d / 'input.frag'; f.write_text(source)
        spv = self.d / 'input.spv'
        subprocess.run([str(self.glslc), '--target-env=vulkan1.1', str(f), '-o', str(spv)], check=True, capture_output=True)
        return spv

    def apply(self, spv):
        out = self.d / 'output.spv'
        out.unlink(missing_ok=True)
        result = subprocess.run([str(self.exe), str(spv), str(out)], capture_output=True, text=True)
        return result, out

    @staticmethod
    def instructions(data):
        words = struct.unpack('<' + 'I' * (len(data) // 4), data)
        at = 5
        while at < len(words):
            count, op = words[at] >> 16, words[at] & 0xffff
            assert count > 0
            yield op, words[at + 1:at + count]
            at += count

    def test_real_fragment_variants_validate_and_preserve_side_effects(self):
        fixture = (ROOT / 'tests/fixtures/alpha.frag').read_text()
        for bound in (0, 1):
            with self.subTest(bound=bound):
                source = fixture.replace('#version 450', '#version 450\n#define BOUND ' + str(bound))
                spv = self.compile(source); before = spv.read_bytes()
                result, out = self.apply(spv)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([str(self.val), '--target-env', 'vulkan1.1', str(out)], check=True, capture_output=True)
                old, new = list(self.instructions(before)), list(self.instructions(out.read_bytes()))
                # AtomicIAdd remains before the helper's discard; EarlyFragmentTests mode is preserved.
                self.assertEqual([p for op, p in new if op == 234], [p for op, p in old if op == 234])
                self.assertEqual([p for op, p in new if op == 16], [p for op, p in old if op == 16])
                self.assertEqual(sum(op == 252 for op, _ in new), sum(op == 252 for op, _ in old) + 1)
                self.assertTrue(any(op == 72 and p[2:] == (35, 16) for op, p in new))
                self.assertTrue(any(op == 72 and p[2:] == (35, 20) for op, p in new))
                self.assertEqual(spv.read_bytes(), before)

    def test_each_normal_fragment_return_runs_alpha_test(self):
        spv = self.compile('#version 450\nlayout(location=0) out vec4 color;\nvoid main(){if(gl_FragCoord.x<3){color=vec4(.25);return;} color=vec4(.75);}\n')
        old = list(self.instructions(spv.read_bytes()))
        returns = sum(op == 253 for op, _ in old)
        self.assertGreaterEqual(returns, 2)
        result, out = self.apply(spv)
        self.assertEqual(result.returncode, 0, result.stderr)
        subprocess.run([str(self.val), '--target-env', 'vulkan1.1', str(out)], check=True, capture_output=True)
        new = list(self.instructions(out.read_bytes()))
        self.assertEqual(sum(op == 57 for op, _ in new) - sum(op == 57 for op, _ in old), returns)

    def test_integer_missing_alpha_and_existing_push_are_refused(self):
        for source, reason in [
            ('layout(location=0) out uvec4 color; void main(){color=uvec4(17);}', 'no float alpha'),
            ('layout(location=1) out vec4 color; void main(){color=vec4(1);}', 'no float alpha'),
            ('layout(location=0) out vec3 color; void main(){color=vec3(1);}', 'no float alpha'),
            ('layout(push_constant) uniform P {float f;} p; layout(location=0) out vec4 color; void main(){color=vec4(p.f);}', 'push constants')]:
            with self.subTest(source=source):
                result, out = self.apply(self.compile('#version 450\n' + source))
                self.assertEqual(result.returncode, 2)
                self.assertIn(reason, result.stderr)
                self.assertFalse(out.exists())

    def test_malformed_module_is_refused(self):
        spv = self.d / 'bad.spv'; spv.write_bytes(struct.pack('<5I', 0x07230203, 0x10300, 0, 10, 0) + bytes(4))
        result, out = self.apply(spv)
        self.assertEqual(result.returncode, 2)
        self.assertIn('invalid', result.stderr)
        self.assertFalse(out.exists())
    def test_alpha_composes_with_runtime_fragment_variants_and_cache(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'))
        src = (DM / 'android/gfx_vk_shader.c').read_text()
        runtime = (DM / 'android/gfx_vk.h').read_text()
        runtime = '\n'.join(l for l in runtime.splitlines() if not any(x in l for x in ('VK_USE_PLATFORM_ANDROID_KHR', '#include <android/', '#include "dm.h"')))
        runtime = runtime.replace('<vulkan/vulkan.h>', '<vulkan/vulkan_core.h>')
        for name in ('vkCreateAndroidSurfaceKHR', 'vkGetAndroidHardwareBufferPropertiesANDROID'):
            runtime = runtime.replace('X(' + name + ')', '')
        helpers = ''.join(function(src, sig) for sig in ('static int spv_is_vec4(', 'static uint32_t *spv_vs_init(', 'static uint32_t *spv_add_outputs(', 'static uint32_t *spv_frag_depth(', 'static uint32_t *spv_dual('))
        integer = src[src.index('typedef struct { const uint32_t *w; uint32_t fn,'):src.index('// A variant of a fragment shader that writes gl_FragDepth')]
        variant = function(src, 'static VkShaderModule vk_shader_module_(vkshader_t *s, uint64_t um, uint64_t im, uint32_t xo, int fd) {')
        spv = self.compile('''#version 450
layout(set=1,binding=0) uniform sampler2D tex;
layout(location=0) out vec4 color;
layout(location=1) out vec4 secondary;
void main(){color=texture(tex,vec2(.5));secondary=vec4(.25);}
''')
        c = self.d / 'variants.c'; exe = self.d / 'variants'
        c.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "runtime.h"
#include "spv_alpha.h"
static pthread_mutex_t g_sh_mu=PTHREAD_MUTEX_INITIALIZER;
vkdev_t V;
static unsigned created;
static char *folder;
#undef VKLOG
#define VKLOG(...) ((void)0)
static VkShaderModule alt_module(vkshader_t *s, int alpha) { (void)alpha; return s->mod; }
static VkResult create(VkDevice d,const VkShaderModuleCreateInfo *c,const VkAllocationCallbacks *a,VkShaderModule *out) {
    (void)d;(void)a;unsigned kills=0;
    for (size_t i=5;i<c->codeSize/4;i+=c->pCode[i]>>16) kills+=(c->pCode[i]&0xffff)==252;
    assert(kills==1);
    char path[2048];snprintf(path,sizeof path,"%s/variant-%u.spv",folder,created);
    FILE *f=fopen(path,"wb");assert(f && fwrite(c->pCode,1,c->codeSize,f)==c->codeSize && !fclose(f));
    *out=(VkShaderModule)(uintptr_t)(2+created++);return VK_SUCCESS;
}
PFN_vkCreateShaderModule vkCreateShaderModule=create;
''' + integer + helpers + variant + r'''
int main(int argc,char **argv) {
    assert(argc==3);folder=argv[2];FILE *f=fopen(argv[1],"rb");assert(f);
    fseek(f,0,SEEK_END);long bytes=ftell(f);rewind(f);uint32_t *w=malloc((size_t)bytes);
    assert(fread(w,1,(size_t)bytes,f)==(size_t)bytes && !fclose(f));
    dmsv_bind binding={.binding=0};vkshader_t s={.stage=1,.mod=(VkShaderModule)(uintptr_t)1,.spv=w,.nspv=(uint32_t)bytes/4,.nbind=1,.b=&binding};
    assert(vk_shader_module_(&s,0,0,0,0)==s.mod && created==0);
    VkShaderModule saved[8];unsigned n=0;
    for (unsigned um=0;um<2;um++) for (int flags=4;flags<8;flags++) {
        VkShaderModule m=vk_shader_module_(&s,um,0,0,flags);
        assert(m!=s.mod && created==n+1);saved[n++]=m;
    }
    n=0;
    for (unsigned um=0;um<2;um++) for (int flags=4;flags<8;flags++) assert(vk_shader_module_(&s,um,0,0,flags)==saved[n++]);
    assert(created==8 && vk_shader_module_(&s,0,0,0,0)==s.mod);free(w);
}
''')
        (self.d / 'runtime.h').write_text(runtime)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function', '-idirafter', str(headers), '-I' + str(DM / 'android'), '-I' + str(DM / 'gfx/nvn'), '-I' + str(DM / 'gfx/nvn/include'), str(c), '-o', str(exe)], check=True, capture_output=True)
        subprocess.run([str(exe), str(spv), str(self.d)], check=True, capture_output=True)
        for variant_path in self.d.glob('variant-*.spv'):
            subprocess.run([str(self.val), '--target-env', 'vulkan1.1', str(variant_path)], check=True, capture_output=True)


class AlphaIntegration(unittest.TestCase):
    def run_c(self, source, headers=None):
        with tempfile.TemporaryDirectory(prefix='vk-alpha-integration-') as td:
            d = Path(td); c = d / 'test.c'; exe = d / 'test'; c.write_text(source)
            cmd = ['cc', '-std=c11', '-Wall', '-Werror', '-Wno-unused-function', '-I' + str(DM / 'gfx/nvn'), '-I' + str(DM / 'gfx/nvn/include'), '-I' + str(DM / 'android')]
            if headers: cmd += ['-idirafter', str(headers)]
            subprocess.run(cmd + [str(c), '-o', str(exe)], check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, capture_output=True)

    def test_public_alpha_reference_records_each_value(self):
        source = (DM / 'gfx/nvn/hle_nvn.c').read_text()
        api = next(l for l in source.splitlines() if l.startswith('NVN(nvnCommandBufferSetAlphaRef,'))
        self.run_c(r'''
#include <assert.h>
#include <stdlib.h>
#include "nvn_obj.h"
static unsigned calls, size;
static unsigned char bytes[16];
void *put(NVNcommandBuffer *cb, unsigned op, unsigned n) { (void)cb; (void)op; calls++; size=n; return bytes; }
#define REC(c, op, T) ((T *)put(c, op, sizeof(T)))
#define NVN(name, ret, ...) ret name(__VA_ARGS__)
#define ONCE_LOG(...) ((void)0)
''' + api + r'''
int main(void) {
    const uint32_t values[]={0x00000000,0x3f000000,0x3f800000,0x7fc00123,0x80000000};
    for (unsigned i=0;i<sizeof values/sizeof *values;i++) {
        float value; memcpy(&value,values+i,4); memset(bytes,0xa5,sizeof bytes);
        nvnCommandBufferSetAlphaRef(NULL,value);
        assert(calls==i+1 && size==4 && !memcmp(bytes,values+i,4));
    }
}
''')

    def test_cached_pipeline_layout_keeps_fragment_alpha_push_range(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers: self.skipTest('NDK Vulkan headers required')
        helper = function((DM / 'android/gfx_vk_shader.c').read_text(), 'VkPipelineLayout vk_pipeline_layout(')
        self.run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <vulkan/vulkan_core.h>
#include "nvn_hmap.h"
#include "spv_alpha.h"
static struct { VkDevice dev; } V;
static unsigned created;
#define VKCHK(call) assert((call)==VK_SUCCESS)
VkResult vkCreateDescriptorSetLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo *c,const VkAllocationCallbacks *a,VkDescriptorSetLayout *out) {
    (void)d;(void)c;(void)a;*out=(VkDescriptorSetLayout)(uintptr_t)1;return VK_SUCCESS;
}
VkResult vkCreatePipelineLayout(VkDevice d,const VkPipelineLayoutCreateInfo *c,const VkAllocationCallbacks *a,VkPipelineLayout *out) {
    (void)d;(void)a;assert(c->pushConstantRangeCount==1);
    assert(c->pPushConstantRanges[0].stageFlags==VK_SHADER_STAGE_FRAGMENT_BIT);
    assert(c->pPushConstantRanges[0].offset==16 && c->pPushConstantRanges[0].size==8);
    *out=(VkPipelineLayout)(uintptr_t)(++created);return VK_SUCCESS;
}
''' + helper + r'''
int main(void) {
    VkDescriptorSetLayout sets[5]={(VkDescriptorSetLayout)(uintptr_t)3,0};
    VkPipelineLayout first=vk_pipeline_layout(sets,2);
    assert(sets[1] && vk_pipeline_layout(sets,2)==first && created==1);
    sets[0]=(VkDescriptorSetLayout)(uintptr_t)4;
    assert(vk_pipeline_layout(sets,1)!=first && created==2);
}
''', headers)

    def test_donor_comparisons_push_values_and_integer_bypass(self):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers: self.skipTest('NDK Vulkan headers required')
        src = (DM / 'android/gfx_vk_exec.c').read_text()
        helpers = src[src.index('enum clear_type {'):src.index('static int prepare_draw(')]
        self.run_c(r'''
#include <assert.h>
#include <math.h>
#include <string.h>
#include <vulkan/vulkan_core.h>
#include "nvn_obj.h"
#include "spv_alpha.h"
typedef struct { int alpha_float; } vkshader_t;
static struct { color_st color; float alpha_ref; } S;
static uint32_t mask; static float reference; static unsigned calls;
void vkCmdPushConstants(VkCommandBuffer cb,VkPipelineLayout layout,VkShaderStageFlags stage,uint32_t off,uint32_t size,const void *p) {
    assert(cb==(VkCommandBuffer)(uintptr_t)1 && layout==(VkPipelineLayout)(uintptr_t)2);
    assert(stage==VK_SHADER_STAGE_FRAGMENT_BIT && off==16 && size==8);
    memcpy(&mask,p,4);memcpy(&reference,(const char *)p+4,4);calls++;
}
''' + helpers + r'''
static int expected(unsigned f,float a,float b) {
    switch (f) {case 1:return 0;case 2:return a<b;case 3:return a==b;case 4:return a<=b;
        case 5:return a>b;case 6:return a!=b;case 7:return a>=b;default:return 1;}
}
int main(void) {
    vkshader_t fs={.alpha_float=1}; const float values[]={-1,0,.3125f,.5f,1,2,NAN};
    for (unsigned f=0;f<=8;f++) {
        S.color.alpha_func=f;
        assert(alpha_enabled(&fs,VK_FORMAT_R8G8B8A8_UNORM,0)==(f>0 && f<8));
        assert(!alpha_enabled(&fs,VK_FORMAT_R8G8B8A8_UNORM,1));
        assert(!alpha_enabled(NULL,VK_FORMAT_R8G8B8A8_UNORM,0));
        for (unsigned r=0;r<7;r++) {
            S.alpha_ref=values[r];alpha_push((VkCommandBuffer)(uintptr_t)1,(VkPipelineLayout)(uintptr_t)2);
            assert(!memcmp(&reference,&S.alpha_ref,4));
            for (unsigned a=0;a<7;a++) {
                float x=values[a];unsigned result=x<reference?1:x==reference?2:x>reference?4:8;
                assert(!!(mask&result)==expected(f,x,reference));
            }
        }
    }
    assert(calls==63);S.color.alpha_func=NVN_ALPHA_FUNC_NEVER;
    for (unsigned i=0;i<2;i++) assert(!alpha_enabled(&fs,i?VK_FORMAT_R32_SINT:VK_FORMAT_R32_UINT,0));
    assert(alpha_enabled(&fs,VK_FORMAT_UNDEFINED,0)); // depth-only draw still has a fragment output
    fs.alpha_float=0;assert(!alpha_enabled(&fs,VK_FORMAT_R8G8B8A8_UNORM,0));
}
''', headers)


if __name__ == '__main__': unittest.main()
