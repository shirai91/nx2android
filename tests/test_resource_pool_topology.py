"""Execute the production pool-layout selection before Vulkan pipeline creation."""
from pathlib import Path
import subprocess,tempfile,unittest
import csrc
ROOT=Path(__file__).resolve().parents[1]
class PoolTopology(unittest.TestCase):
    def test_geometry_topology_changes_without_rebinding(self):
        source=csrc.unit(ROOT/'nxnative/dm/android/gfx_vk_exec.c')
        begin=source.index('    // layout and pipeline',source.index('static int prepare_draw('))
        chunk=source[begin:source.index('    VkDescriptorSetLayout dsl[5]',begin)]
        with tempfile.TemporaryDirectory(prefix='nx2a-pool-topology-') as tmp:
            c=Path(tmp)/'topology.c';exe=Path(tmp)/'topology'
            c.write_text('''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef unsigned VkPrimitiveTopology;
typedef struct{uint64_t key;unsigned has_pools,pool_variant;}vkshader_t;
static struct{vkshader_t *sh[3];}S;
static vkshader_t original={10,1,0},points={20,1,1},lines={30,1,2},*g_pool_gs_source=&original;
static vkshader_t *vk_shader_gs_topo(vkshader_t *s,VkPrimitiveTopology t){assert(s==&original);return t==0?&points:&lines;}
static int prepare_resource_pools(int st,vkshader_t **s){assert(st==2);return 1;}
static int vk_shader_pool_limits(vkshader_t *const *stages,int n){return 1;}
#define VKRED_ONCE(...) ((void)0)
static int pick(unsigned topology){struct{unsigned topo,raster_discard,has_pools,pool_gs,pool_variant[3];uint64_t key[3];}k={.topo=topology};
'''+chunk+'''
return k.key[2]==S.sh[2]->key && k.pool_variant[2]==S.sh[2]->pool_variant && k.has_pools && k.pool_gs;
}
int main(void){S.sh[2]=&original;assert(pick(0));assert(S.sh[2]==&points);assert(pick(1));assert(S.sh[2]==&lines);assert(pick(0));assert(S.sh[2]==&points);points.has_pools=0;assert(pick(0));assert(S.sh[2]==&points);original.has_pools=0;points.has_pools=1;assert(pick(0));assert(S.sh[2]==&points);lines.has_pools=0;assert(!pick(1));assert(S.sh[2]==&original);return 0;}
''')
            subprocess.run(['cc','-std=c11',str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

    def test_geometry_split_uses_bound_source_on_repeated_draws(self):
        source=csrc.unit(ROOT/'nxnative/dm/android/gfx_vk_exec.c')
        shader=(ROOT/'nxnative/dm/android/gfx_vk_shader.c').read_text()
        begin=source.index('    // layout and pipeline',source.index('static int prepare_draw('))
        prepare=source[begin:source.index('    VkDescriptorSetLayout dsl[5]',begin)]
        code=(ROOT/'tests/resource_pool_topology.c').read_text()
        code=code.replace('/* TOPOLOGY */',csrc.function(shader,'vkshader_t *vk_shader_gs_topo('))
        code=code.replace('/* SPLIT */',csrc.function(source,'static int gs_split_draw('))
        code=code.replace('/* PREPARE */',prepare)
        with tempfile.TemporaryDirectory(prefix='nx2a-pool-split-') as tmp:
            c=Path(tmp)/'split.c';exe=Path(tmp)/'split';c.write_text(code)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra',str(c),'-o',str(exe)],check=True)
            for base_pool,point_pool in ((0,1),(1,0),(0,0),(1,1)):
                with self.subTest(base_pool=base_pool,point_pool=point_pool):
                    result=subprocess.run([str(exe),str(base_pool),str(point_pool)],capture_output=True,text=True)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
