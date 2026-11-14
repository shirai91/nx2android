"""Portable native mocks for packaged default-off query modules;no driver/device."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

SKILL = Path(__file__).resolve().parents[1]


def harness_source():
    prefix = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct { VkDevice dev;uint32_t range_timestamp_bits;int has_shader_work;struct{struct{float timestampPeriod;}limits;}props; } V;
static unsigned creates,resets,writes,gets,begins,ends,range_rows,work_rows,fail_get;
static void logger(const char *format,...) {
 char line[1024];va_list a;va_start(a,format);vsnprintf(line,sizeof line,format,a);va_end(a);
 range_rows+=strstr(line,"GPU range row:")!=NULL;work_rows+=strstr(line,"shader workload row:")!=NULL;
}
#define VKLOG(...) logger(__VA_ARGS__)
static VkResult createq(VkDevice d,const VkQueryPoolCreateInfo*c,const VkAllocationCallbacks*a,VkQueryPool*p){
 (void)d;(void)a;creates++;assert(c->queryCount==(c->queryType==VK_QUERY_TYPE_TIMESTAMP?1536u:24576u));
 *p=(VkQueryPool)(uintptr_t)(9+c->queryType);return VK_SUCCESS;
}
static void resetq(VkCommandBuffer cb,VkQueryPool p,uint32_t q,uint32_t n){assert(cb&&p&&n);(void)q;resets++;}
static void writeq(VkCommandBuffer cb,VkPipelineStageFlagBits s,VkQueryPool p,uint32_t q){assert(cb&&p&&(s==1||s==8192)&&q<1536);writes++;}
static VkResult getq(VkDevice d,VkQueryPool p,uint32_t first,uint32_t n,size_t bytes,void*out,VkDeviceSize stride,VkQueryResultFlags flags){
 (void)d;(void)p;(void)first;assert(flags==VK_QUERY_RESULT_64_BIT&&bytes==stride*n);gets++;if(fail_get)return VK_NOT_READY;
 uint64_t*v=out;for(unsigned i=0;i<bytes/8;i++)v[i]=1000+i*10;return VK_SUCCESS;
}
static void beginq(VkCommandBuffer cb,VkQueryPool p,uint32_t q,VkQueryControlFlags flags){(void)q;assert(cb&&p&&!flags);begins++;}
static void endq(VkCommandBuffer cb,VkQueryPool p,uint32_t q){(void)q;assert(cb&&p);ends++;}
#define vkCreateQueryPool createq
#define vkCmdResetQueryPool resetq
#define vkCmdWriteTimestamp writeq
#define vkGetQueryPoolResults getq
#define vkCmdBeginQuery beginq
#define vkCmdEndQuery endq
'''
    modules = "\n".join((SKILL / "runtime" / name).read_text().replace('#include "gfx_vk.h"', '')
                        for name in ("gfx_vk_range_diag.c", "gfx_vk_shader_work.c"))
    suffix = r'''
int main(void){
 VkCommandBuffer cb=(VkCommandBuffer)1;V.range_timestamp_bits=36;V.props.limits.timestampPeriod=1;V.has_shader_work=1;
 vk_rt_part(cb,0,0,10);vk_sw_part(cb,0,0,10);assert(!creates&&!resets&&!writes);
 setenv("DM_VK_RANGE_FIRST","0",1);setenv("DM_VK_RANGE_LAST","1",1);setenv("DM_VK_RANGE_TIMES","3",1);
 vk_rt_part(cb,0,0,10);vk_rt_part(cb,1,0,11);vk_rt_mark(cb,0,0,32,16,0);vk_rt_mark(cb,0,7,32,16,3);
 assert(writes==2&&resets==1);vk_rt_complete(11,1);assert(gets==1&&range_rows==2);vk_rt_complete(11,1);assert(gets==1);
 setenv("DM_VK_RANGE_TIMES","6",1);vk_rt_part(cb,0,0,12);vk_rt_part(cb,2,0,13);
 unsigned oldw=writes,oldr=resets,oldg=gets;vk_rt_mark(cb,0,0,32,16,0);
 for(unsigned i=1;i<=1025;i++)vk_rt_draw(cb,0,i,i,32,16,1,2,1,1,4);
 assert(writes==oldw&&resets==oldr&&g_rt[2].ndraw==1024&&g_rt[2].dropped==1);vk_rt_complete(13,2);assert(gets==oldg);
 setenv("DM_VK_RANGE_TIMES","3",1);vk_rt_part(cb,0,0,14);vk_rt_part(cb,3,0,15);vk_rt_mark(cb,0,0,32,16,0);
 fail_get=1;vk_rt_complete(15,3);fail_get=0;oldg=gets;vk_rt_complete(15,3);assert(gets==oldg);
 setenv("DM_VK_SHADER_WORK_FIRST","0",1);setenv("DM_VK_SHADER_WORK_LAST","1",1);setenv("DM_VK_SHADER_WORK","1",1);
 vk_sw_part(cb,0,0,20);vk_sw_part(cb,1,0,21);assert(!vk_sw_begin(cb,1,2,2,1,32,16,1,1,4));
 uint32_t q=vk_sw_begin(cb,1,2,0,1,32,16,1,1,4);assert(q);assert(!vk_sw_begin(cb,1,2,0,2,32,16,1,1,4));vk_sw_end(cb,q);
 vk_sw_complete(20,1);oldg=gets;vk_sw_complete(21,1);assert(gets==oldg+1&&work_rows==1&&begins==ends);
 oldg=gets;vk_sw_complete(21,1);assert(gets==oldg);
 setenv("DM_VK_SHADER_WORK","1",1);vk_sw_part(cb,0,0,22);vk_sw_part(cb,2,0,23);
 for(unsigned i=0;i<4097;i++){q=vk_sw_begin(cb,1,2,0,i,32,16,1,1,4);if(q)vk_sw_end(cb,q);}
 assert(g_sw[2].count==4096&&g_sw[2].dropped==1);vk_sw_complete(23,2);assert(work_rows==4097&&begins==ends);
 puts("PASS default-off,slot/frame lifecycle,overflow,failure,no-timestamp census,actual draw query association");return 0;
}
'''
    return prefix + modules + suffix


class NativeModules(unittest.TestCase):
    def test_native_lifecycle(self):
        compiler = os.environ.get("GPU_PROFILE_CC") or shutil.which("cc")
        if not compiler:
            self.skipTest("POSIX host C compiler unavailable;use harness_source for explicit NDK compile")
        headers = SKILL.parents[2] / "nxnative/dm/third_party/vulkan-headers/include"
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            source = path / "test.c"
            source.write_text(harness_source(), encoding="utf-8")
            executable = path / ("test.exe" if os.name == "nt" else "test")
            build = subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                                    "-Wno-missing-field-initializers", "-I"+str(headers), str(source), "-o", str(executable)],
                                   capture_output=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stderr.decode(errors="replace"))
            run = subprocess.run([str(executable)], capture_output=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stderr.decode(errors="replace"))


if __name__ == "__main__":
    unittest.main()
