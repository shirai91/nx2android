/* The production topology resolver, split decision and pool-layout selection run
 * together. Vulkan services are modeled: pools prevent splitting, direct buffers
 * allow it. Reusing a topology-derived key must not change which shader is tested. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned VkPrimitiveTopology;
enum { VK_PRIMITIVE_TOPOLOGY_POINT_LIST=0,VK_PRIMITIVE_TOPOLOGY_LINE_LIST=1,VK_PRIMITIVE_TOPOLOGY_LINE_STRIP=2,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST=3,VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY=6,VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY=7,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY=8,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY=9 };
typedef struct { uint64_t key; unsigned has_pools,pool_variant,xfb,xfb_stride[4],in_locs,out_locs; } vkshader_t;
static vkshader_t original={.key=10},points={.key=10^0x9E3779B97F4A7C15ull},vertex={0};
static vkshader_t *g_pool_gs_source=&original,*examined,*alias_checked;
static struct { vkshader_t *sh[3]; } S;
static int g_gs_split=1,g_gs_split_mode,g_tf_on,g_bc_slot=-1,g_spv_alt,quirk=1,alias_safe=1;
#define VQ_GS_SPLIT 1
#define VKRED_ONCE(...) ((void)0)
#define VKLOG(...) ((void)0)
static int vk_quirk(int q) { assert(q==VQ_GS_SPLIT);return quirk; }
static int shader_wmem(vkshader_t *s) { assert(s==&vertex);return 0; }
static unsigned topo(int p) { return (unsigned)p; }
static vkshader_t *vk_shader_q(uint64_t key,int stage,int quiet) {
    assert(stage==2 && quiet);return key==original.key?&original:key==points.key?&points:NULL;
}
static int vk_shader_gs_split_ready(vkshader_t *s) { examined=s;return !s->has_pools; }
static int gs_split_inputs_safe(vkshader_t *s,uint64_t ibuf,uint64_t in) {
    assert(ibuf==32 && in==16);alias_checked=s;return alias_safe;
}
static int prepare_resource_pools(int st,vkshader_t **s) { assert(st==0 || st==2);assert(*s);return 1; }
static int vk_shader_pool_limits(vkshader_t *const *s,int n) { assert(n==3 && s[2]);return 1; }
/* TOPOLOGY */
/* SPLIT */
static int prepare(unsigned topology) {
    struct { unsigned topo,raster_discard,has_pools,pool_gs,pool_variant[3];uint64_t key[3]; } k={.topo=topology};
/* PREPARE */
    assert(k.key[2]==S.sh[2]->key);return 1;
}
static void check(unsigned topology,vkshader_t *expected) {
    examined=alias_checked=NULL;
    assert(gs_split_draw((int)topology,0,32,16)==!expected->has_pools);
    assert(examined==expected);
    assert(alias_checked==(!expected->has_pools?expected:NULL));
    assert(prepare(topology));
}
int main(int argc,char **argv) {
    assert(argc==3);original.has_pools=(unsigned)atoi(argv[1]);points.has_pools=(unsigned)atoi(argv[2]);
    S.sh[0]=&vertex;S.sh[2]=&original;
    for(int i=0;i<3;i++)check(VK_PRIMITIVE_TOPOLOGY_POINT_LIST,&points);
    check(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,&original);
    check(VK_PRIMITIVE_TOPOLOGY_POINT_LIST,&points);
    check(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,&original);
    quirk=0;examined=NULL;assert(!gs_split_draw(0,0,32,16) && !examined);quirk=1;
    assert(!gs_split_draw(0,1,32,16));
    points.has_pools=0;alias_safe=0;assert(!gs_split_draw(0,0,32,16));assert(alias_checked==&points);
    /* The ordinary no-pool path also works when no pool source was retained. */
    alias_safe=1;original.has_pools=0;g_pool_gs_source=NULL;S.sh[2]=&original;
    check(VK_PRIMITIVE_TOPOLOGY_POINT_LIST,&points);check(VK_PRIMITIVE_TOPOLOGY_POINT_LIST,&points);
    return 0;
}
