"""Execute the production pool-layout selection before Vulkan pipeline creation."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class PoolTopology(unittest.TestCase):
    def test_geometry_topology_changes_without_rebinding(self):
        source=(ROOT/'nxnative/dm/android/gfx_vk_exec.c').read_text()
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
