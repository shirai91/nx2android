"""Existing donor logic eligibility and blend precedence, using production C."""
import os,re,shutil,subprocess,tempfile,unittest
from pathlib import Path
W=Path(__file__).resolve().parents[1]
class LogicOperations(unittest.TestCase):
 def test_donor_format_and_state_limits(self):
  helper=W/'nxnative/dm/android/gfx_vk_logic.inc'
  self.assertTrue(helper.exists(),'existing logic selection not ported')
  ndk=os.environ.get('ANDROID_NDK_HOME')
  if not ndk:self.skipTest('ANDROID_NDK_HOME required for Vulkan format enum')
  headers=list(Path(ndk).glob('toolchains/llvm/prebuilt/*/sysroot/usr/include/vulkan/vulkan_core.h'))
  if not headers:self.skipTest('NDK Vulkan header unavailable')
  values=re.findall(r'(VK_FORMAT_\w+)\s*=\s*(\d+),',headers[0].read_text())
  runtime=(W/'nxnative/dm/android/gfx_vk_exec.c').read_text()
  classify=runtime[runtime.index('enum clear_type {'):runtime.index('static int alpha_enabled(')]
  enums='enum {'+','.join(name+'='+value for name,value in values)+'};'
  code=r'''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t VkFormat;
#define NVN_LOGIC_OP_COPY 3
#define VKRED_ONCE(...) ((void)0)
'''+enums+r'''
typedef struct {uint32_t ncol,cfmt[8],wmask[8],blend_en,logic_op,logic_en,alpha;} pkey;
static struct {int has_logic_op;} V={1};
static struct {void *sh[3];struct {unsigned alpha_func;} color;} S={.sh={0,(void*)1,0}};
'''+classify+helper.read_text()+r'''
static pkey setup(void) {return (pkey){.ncol=1,.cfmt={VK_FORMAT_R8G8B8A8_UNORM},.wmask={15},.blend_en=1};}
int main(void) {
 for(unsigned op=0;op<16;op++){pkey k=setup();k.logic_op=op;color_logic(&k);assert(k.logic_en==(op!=3));assert(k.blend_en==(op==3));}
 pkey k=setup();k.logic_op=6;V.has_logic_op=0;color_logic(&k);assert(!k.logic_en && !k.blend_en);V.has_logic_op=1;
 k=setup();k.logic_op=6;k.alpha=1;color_logic(&k);assert(!k.logic_en && k.blend_en);
 k=setup();k.logic_op=6;k.ncol=2;k.cfmt[1]=VK_FORMAT_R32_SFLOAT;k.wmask[1]=15;color_logic(&k);assert(!k.logic_en);
 k=setup();k.logic_op=6;k.ncol=2;k.cfmt[1]=VK_FORMAT_R32_SFLOAT;k.wmask[1]=0;color_logic(&k);assert(k.logic_en);
 const VkFormat eligible[]={VK_FORMAT_R8_UINT,VK_FORMAT_R16G16B16A16_UINT,VK_FORMAT_R32G32_UINT,VK_FORMAT_A2B10G10R10_UINT_PACK32,VK_FORMAT_R16_UNORM,VK_FORMAT_R5G6B5_UNORM_PACK16,VK_FORMAT_B8G8R8A8_UNORM};
 for(unsigned i=0;i<sizeof eligible/sizeof *eligible;i++){k=setup();k.logic_op=6;k.cfmt[0]=eligible[i];color_logic(&k);assert(k.logic_en && !k.blend_en);}
 const VkFormat excluded[]={VK_FORMAT_R8_SINT,VK_FORMAT_R16_SFLOAT,VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_R8_SNORM};
 for(unsigned i=0;i<sizeof excluded/sizeof *excluded;i++){k=setup();k.logic_op=6;k.cfmt[0]=excluded[i];color_logic(&k);assert(!k.logic_en);}
 k=setup();k.logic_op=6;k.wmask[0]=0;color_logic(&k);assert(!k.logic_en);
 k=setup();k.logic_op=6;S.sh[1]=NULL;color_logic(&k);assert(!k.logic_en);
 puts("33 native logic eligibility/state cases passed");
}
'''
  cc=shutil.which('clang')
  if not cc:self.skipTest('clang required')
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'test.c').write_text(code)
   result=subprocess.run([cc,'-std=c11','-O1','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],capture_output=True,text=True)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   result=subprocess.run([str(p/'test')],capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
 def test_persisted_enabled_keys_respect_current_device(self):
  source=(W/'nxnative/dm/android/gfx_vk_exec.c').read_text()
  a=source.index('static void *prewarm_thread(');b=source.index('\n}',a)+2
  code=r'''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#define pthread_setname_np(...) ((void)0)
#define setpriority(...) ((void)0)
#define VKLOG(...) ((void)0)
typedef void *VkPipeline;typedef void *VkPipelineLayout;typedef void *VkDescriptorSetLayout;
typedef struct {uint64_t key[3];unsigned logic_en,raster_discard;} pkey;
typedef struct {unsigned set;VkDescriptorSetLayout dsl;} vkshader_t;
static pkey saved[2]={{.key={1},.logic_en=1},{.key={2}}},*g_pw=saved;
static size_t g_pw_n=2,g_pw_next,g_pw_done;
static int g_pipes,built,looked;static pthread_mutex_t g_pipe_mu=PTHREAD_MUTEX_INITIALIZER;
static struct {int has_logic_op;void *dev;} V;
static void *nvn_hm_get(void *map,const void *key){(void)map;(void)key;return NULL;}
static void nvn_hm_put(void *map,const void *key,void *value){(void)map;(void)key;(void)value;}
static vkshader_t *vk_shader(uint64_t key,int stage){(void)key;(void)stage;static vkshader_t s={0,(void*)1};looked++;return &s;}
static VkPipelineLayout vk_pipeline_layout(VkDescriptorSetLayout *sets,int n){(void)sets;(void)n;return (void*)2;}
static VkPipeline build_pipeline(const pkey *k,VkPipelineLayout layout,vkshader_t *const *stages){
 (void)layout;(void)stages;assert(V.has_logic_op || !k->logic_en);built++;return (void*)3;
}
static void vkDestroyPipeline(void *dev,VkPipeline p,void *a){(void)dev;(void)p;(void)a;assert(0);}
'''+source[a:b]+r'''
int main(void){
 prewarm_thread(NULL);assert(built==1 && looked==1);
 V.has_logic_op=1;built=looked=0;g_pw_next=g_pw_done=0;
 prewarm_thread(NULL);assert(built==2 && looked==2);
}
'''
  cc=shutil.which('clang')
  if not cc:self.skipTest('clang required')
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'test.c').write_text(code)
   result=subprocess.run([cc,'-std=c11','-O1','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],capture_output=True,text=True)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   result=subprocess.run([str(p/'test')],capture_output=True,text=True);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
if __name__=='__main__':unittest.main()
