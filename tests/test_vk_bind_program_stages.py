"""The Vulkan replay's bind_program: a program without a vertex shader bound with the vertex bit (BindProgram(ps, 0x1f), Little Busters! Converted Edition)
keeps the vertex stage; NULL, vertex-only, compute-only and partly unknown programs change every stage they name."""
from pathlib import Path
import shutil, subprocess, tempfile, unittest
import csrc


def production():
    inc = csrc.read('nxnative/dm/android/gfx_vk_exec_pipelines.inc')
    key64 = next(line for line in inc.splitlines() if line.startswith('static uint64_t key64('))
    cut = key64 + '\n' + ''.join(csrc.function(inc, head, 'gfx_vk_exec_pipelines.inc') for head in (
        'static uint32_t prog_stage_bits(', 'static void bind_program(const p_bind_program *bp) {'))
    return r'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef struct { int nstage; uint64_t key[6]; int stage[6]; } prog_t;   /* stand-in: each shader's key and the stage the bundle gives it (-1: missing) */
typedef struct { const prog_t *prog; int32_t stages; } p_bind_program;
typedef struct { uint64_t key; } vkshader_t;
static vkshader_t shaders[64]; static int nshaders;
static const prog_t *all[8]; static int nall;
static int prog_key(const prog_t *p, unsigned i, uint8_t k[8]) { if (i >= (unsigned)p->nstage) return 0; for (int b = 0; b < 8; b++) k[b] = (uint8_t)(p->key[i] >> (56 - 8 * b)); return 1; }
int vk_shader_stage(uint64_t key) { for (int i = 0; i < nall; i++) for (int j = 0; j < all[i]->nstage; j++) if (all[i]->key[j] == key) return all[i]->stage[j]; return -1; }
static vkshader_t *vk_shader(uint64_t key, int st) { (void)st; shaders[nshaders].key = key; return &shaders[nshaders++]; }
static int missing; void vk_shader_missing(uint64_t key) { (void)key; missing++; }
static void shader_dump(const prog_t *p, unsigned i, uint64_t kk) { (void)p; (void)i; (void)kk; }
static struct { vkshader_t *sh[6]; const prog_t *prog; int prog_ok; } S;
''' + cut + r'''
static uint64_t key(int st) { return S.sh[st] ? S.sh[st]->key : 0; }
static void bind(const prog_t *p, int stages) { p_bind_program bp = {p, stages}; bind_program(&bp); }
int main(void) {
 prog_t vs = {1, {0x11}, {0}}, ps = {1, {0x22}, {1}}, vsps = {2, {0x31, 0x32}, {0, 1}}, gs = {1, {0x44}, {2}}, cs = {1, {0x55}, {5}}, bad = {1, {0x66}, {-1}};
 const prog_t *ps_and_missing_p; prog_t ps_and_missing = {2, {0x22, 0x77}, {1, -1}}; ps_and_missing_p = &ps_and_missing;
 all[nall++] = &vs; all[nall++] = &ps; all[nall++] = &vsps; all[nall++] = &gs; all[nall++] = &cs; all[nall++] = &bad; all[nall++] = ps_and_missing_p;
 bind(&vs, 0x1f); bind(&ps, 0x1f);             /* Little Busters!: one program per shader, both with all graphics bits */
 assert(key(0) == 0x11 && key(1) == 0x22 && S.prog_ok);
 bind(&gs, 0x1f); assert(key(0) == 0x11 && key(1) == 0 && key(2) == 0x44);   /* geometry-only keeps the vertex stage, still unbinds the absent fragment */
 bind(&vsps, 0x1f); bind(&vs, 0x1f);           /* Xenoblade Chronicles 2: a vertex-only program after vertex + fragment unbinds the fragment */
 assert(key(0) == 0x11 && key(1) == 0);
 bind(&vsps, 0x1f); bind(&ps, 0x02); assert(key(0) == 0x31 && key(1) == 0x22);
 bind(&vsps, 0x1f); bind(NULL, 0x1f); assert(!key(0) && !key(1) && !S.prog_ok);   /* NULL unbinds everything it names */
 bind(&vs, 0x1f); bind(&cs, 0x3f); assert(!key(0) && key(5) == 0x55);           /* compute-only changes every stage it names */
 bind(&vs, 0x1f); bind(&bad, 0x1f); assert(!key(0) && missing == 1);           /* a program whose stage is not known does not keep the vertex stage */
 bind(&vs, 0x1f); bind(ps_and_missing_p, 0x1f); assert(!key(0) && key(1) == 0x22);
 puts("bind_program stages: PASS"); return 0;
}
'''


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class VkBindProgramStages(unittest.TestCase):
    def test_vertex_stage_kept_for_pixel_only_program(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / 'bind.c'; exe = Path(td) / 'bind'; src.write_text(production(), encoding='utf-8')
            r = subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', str(src), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('PASS', subprocess.check_output([str(exe)], text=True))


if __name__ == '__main__': unittest.main()
