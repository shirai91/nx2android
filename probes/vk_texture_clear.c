// Used by vk_texture_clear.py. The marked sections come from production source.
/* TYPES */
#include "nvn_hmap.h"
#include "vk_compressed_clear.h"
#include <math.h>
#include <stdarg.h>
#include "clear_sample_array_comp.spv.h"
#include "clear_sample_volume_comp.spv.h"
#include "maskclear_vert.spv.h"
#include "maskclear_frag.spv.h"
#include "maskclear_uint_frag.spv.h"
#include "maskclear_sint_frag.spv.h"
#define CHECK(call) do { VkResult result=(call); if(result) {fprintf(stderr,"%s: %d\n",#call,result);exit(1);} } while(0)
static int allow_compressed_fallback, fallback_diagnostics;
static void diagnostic(int id,const char *format,...) {
    va_list args;va_start(args,format);
    if(allow_compressed_fallback && id==71 && strstr(format,"compressed") && strstr(format,"whole")) {
        fallback_diagnostics++;va_end(args);return;
    }
    fprintf(stderr,"[RED] ");vfprintf(stderr,format,args);fputc('\n',stderr);va_end(args);exit(1);
}
#define VKRED_ONCE(id, ...) diagnostic(id,__VA_ARGS__)
static struct { VkDevice dev; VkPhysicalDevice pd; VkPipelineCache pcache; int has_dynrender; } V;
static struct { VkPipeline bound_pipe; } S;
static int g_bc_stale, legacy;
static vkimg_t image;
static VkFormat vk_view_fmt;
static VkCommandBuffer command;
static VkQueue queue;
static VkImageView views[64]; static unsigned nviews;
static VkFramebuffer framebuffers[64]; static unsigned nfbs;
static PFN_vkCmdBeginRendering begin_rendering;
static PFN_vkCmdEndRendering end_rendering;
#define vkCmdBeginRendering begin_rendering
#define vkCmdEndRendering end_rendering
static int rp_on(void) {return legacy;}
static VkRenderPass passes[5];
static const vkfmt_t formats[] = {
    {.vk=VK_FORMAT_R32G32B32A32_SFLOAT,.is_int=0,.is_sint=0,.compressed=0,.bw=1,.bh=1,.bpb=16},
    {.vk=VK_FORMAT_R32G32B32A32_UINT,.is_int=1,.is_sint=0,.compressed=0,.bw=1,.bh=1,.bpb=16},
    {.vk=VK_FORMAT_R32G32B32A32_SINT,.is_int=1,.is_sint=1,.compressed=0,.bw=1,.bh=1,.bpb=16},
    {.vk=VK_FORMAT_R8G8B8A8_UNORM,.is_int=0,.is_sint=0,.compressed=0,.bw=1,.bh=1,.bpb=4},
    {.vk=VK_FORMAT_R8G8B8A8_SRGB,.is_int=0,.is_sint=0,.compressed=0,.bw=1,.bh=1,.bpb=4},
    {.vk=VK_FORMAT_BC1_RGB_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=8},
    {.vk=VK_FORMAT_BC1_RGB_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=8},
    {.vk=VK_FORMAT_BC1_RGBA_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=8},
    {.vk=VK_FORMAT_BC1_RGBA_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=8},
    {.vk=VK_FORMAT_BC2_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_BC2_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_BC3_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_BC3_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_BC4_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=8},
    {.vk=VK_FORMAT_BC4_SNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=8},
    {.vk=VK_FORMAT_BC5_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_BC5_SNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_BC6H_UFLOAT_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_BC6H_SFLOAT_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_BC7_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_BC7_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_ASTC_4x4_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_ASTC_4x4_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=4,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_ASTC_5x4_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=5,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_ASTC_5x4_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=5,.bh=4,.bpb=16},
    {.vk=VK_FORMAT_ASTC_5x5_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=5,.bh=5,.bpb=16},
    {.vk=VK_FORMAT_ASTC_5x5_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=5,.bh=5,.bpb=16},
    {.vk=VK_FORMAT_ASTC_6x5_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=6,.bh=5,.bpb=16},
    {.vk=VK_FORMAT_ASTC_6x5_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=6,.bh=5,.bpb=16},
    {.vk=VK_FORMAT_ASTC_6x6_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=6,.bh=6,.bpb=16},
    {.vk=VK_FORMAT_ASTC_6x6_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=6,.bh=6,.bpb=16},
    {.vk=VK_FORMAT_ASTC_8x5_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=8,.bh=5,.bpb=16},
    {.vk=VK_FORMAT_ASTC_8x5_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=8,.bh=5,.bpb=16},
    {.vk=VK_FORMAT_ASTC_8x6_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=8,.bh=6,.bpb=16},
    {.vk=VK_FORMAT_ASTC_8x6_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=8,.bh=6,.bpb=16},
    {.vk=VK_FORMAT_ASTC_8x8_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=8,.bh=8,.bpb=16},
    {.vk=VK_FORMAT_ASTC_8x8_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=8,.bh=8,.bpb=16},
    {.vk=VK_FORMAT_ASTC_10x5_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=10,.bh=5,.bpb=16},
    {.vk=VK_FORMAT_ASTC_10x5_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=10,.bh=5,.bpb=16},
    {.vk=VK_FORMAT_ASTC_10x6_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=10,.bh=6,.bpb=16},
    {.vk=VK_FORMAT_ASTC_10x6_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=10,.bh=6,.bpb=16},
    {.vk=VK_FORMAT_ASTC_10x8_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=10,.bh=8,.bpb=16},
    {.vk=VK_FORMAT_ASTC_10x8_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=10,.bh=8,.bpb=16},
    {.vk=VK_FORMAT_ASTC_10x10_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=10,.bh=10,.bpb=16},
    {.vk=VK_FORMAT_ASTC_10x10_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=10,.bh=10,.bpb=16},
    {.vk=VK_FORMAT_ASTC_12x10_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=12,.bh=10,.bpb=16},
    {.vk=VK_FORMAT_ASTC_12x10_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=12,.bh=10,.bpb=16},
    {.vk=VK_FORMAT_ASTC_12x12_UNORM_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=12,.bh=12,.bpb=16},
    {.vk=VK_FORMAT_ASTC_12x12_SRGB_BLOCK,.is_int=0,.is_sint=0,.compressed=1,.bw=12,.bh=12,.bpb=16},
};
static const char *format_names[] = {
    "R32G32B32A32_SFLOAT",
    "R32G32B32A32_UINT",
    "R32G32B32A32_SINT",
    "R8G8B8A8_UNORM",
    "R8G8B8A8_SRGB",
    "BC1_RGB_UNORM_BLOCK",
    "BC1_RGB_SRGB_BLOCK",
    "BC1_RGBA_UNORM_BLOCK",
    "BC1_RGBA_SRGB_BLOCK",
    "BC2_UNORM_BLOCK",
    "BC2_SRGB_BLOCK",
    "BC3_UNORM_BLOCK",
    "BC3_SRGB_BLOCK",
    "BC4_UNORM_BLOCK",
    "BC4_SNORM_BLOCK",
    "BC5_UNORM_BLOCK",
    "BC5_SNORM_BLOCK",
    "BC6H_UFLOAT_BLOCK",
    "BC6H_SFLOAT_BLOCK",
    "BC7_UNORM_BLOCK",
    "BC7_SRGB_BLOCK",
    "ASTC_4x4_UNORM_BLOCK",
    "ASTC_4x4_SRGB_BLOCK",
    "ASTC_5x4_UNORM_BLOCK",
    "ASTC_5x4_SRGB_BLOCK",
    "ASTC_5x5_UNORM_BLOCK",
    "ASTC_5x5_SRGB_BLOCK",
    "ASTC_6x5_UNORM_BLOCK",
    "ASTC_6x5_SRGB_BLOCK",
    "ASTC_6x6_UNORM_BLOCK",
    "ASTC_6x6_SRGB_BLOCK",
    "ASTC_8x5_UNORM_BLOCK",
    "ASTC_8x5_SRGB_BLOCK",
    "ASTC_8x6_UNORM_BLOCK",
    "ASTC_8x6_SRGB_BLOCK",
    "ASTC_8x8_UNORM_BLOCK",
    "ASTC_8x8_SRGB_BLOCK",
    "ASTC_10x5_UNORM_BLOCK",
    "ASTC_10x5_SRGB_BLOCK",
    "ASTC_10x6_UNORM_BLOCK",
    "ASTC_10x6_SRGB_BLOCK",
    "ASTC_10x8_UNORM_BLOCK",
    "ASTC_10x8_SRGB_BLOCK",
    "ASTC_10x10_UNORM_BLOCK",
    "ASTC_10x10_SRGB_BLOCK",
    "ASTC_12x10_UNORM_BLOCK",
    "ASTC_12x10_SRGB_BLOCK",
    "ASTC_12x12_UNORM_BLOCK",
    "ASTC_12x12_SRGB_BLOCK",
};
static const vkfmt_t *vk_fmt(int f) {return &formats[f];}
static VkRenderPass rp_compat(uint32_t ncol,const uint32_t *cfmt,uint32_t depth,uint32_t samples) {
    (void)ncol;(void)depth;
    unsigned i=0;while(i<5 && formats[i].vk!=(VkFormat)cfmt[0])i++;
    if(i==5)exit(1);
    if(!passes[i]) {
        VkAttachmentDescription a={0,(VkFormat)cfmt[0],samples,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL};
        VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ref};
        VkRenderPassCreateInfo ci={VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,NULL,0,1,&a,1,&sub,0,NULL};
        CHECK(vkCreateRenderPass(V.dev,&ci,NULL,&passes[i]));
    }
    return passes[i];
}
// This isolated probe has no disk pipeline cache to mark dirty.
static void vk_pipeline_cache_changed(void) {}
/* PIPELINE */
static vkimg_t *vk_tex(const tex_t *t) {(void)t;return &image;}
static void vk_end_pass(void) {}
static VkCommandBuffer vk_cmd(void) {return command;}
static void vk_tex_prepare(VkCommandBuffer cb,vkimg_t *im) {(void)cb;(void)im;}
static void vk_img_barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier b={VK_STRUCTURE_TYPE_MEMORY_BARRIER,NULL,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,NULL,0,NULL);
}
static void vk_defer_free_framebuffer(VkFramebuffer fb) {if(nfbs==64)abort();framebuffers[nfbs++]=fb;}
static VkImageView vk_tex_view(const tex_t *t,const view_t *v,int dim,int arrayed,int attachment,vkimg_t **out) {
    (void)dim;(void)arrayed;(void)attachment;(void)out;
    vk_view_fmt=vk_fmt(v->set_format ? v->format : t->d.format)->vk;
    VkImageViewCreateInfo ci={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,NULL,0,image.img,
        image.target==NVN_TEXTURE_TARGET_3D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        vk_view_fmt,{0,0,0,0},{VK_IMAGE_ASPECT_COLOR_BIT,v->base_level,1,v->min_layer,1}};
    if(nviews==64)abort();CHECK(vkCreateImageView(V.dev,&ci,NULL,&views[nviews]));return views[nviews++];
}
static void *vk_scratch(VkDeviceSize size,VkDeviceSize alignment,VkBuffer *buffer,VkDeviceSize *offset);
/* CLEAR */
static uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(V.pd,&p);
    for(uint32_t i=0;i<p.memoryTypeCount;i++)if((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags)return i;
    fprintf(stderr,"No compatible memory type\n");exit(1);
}
static VkDeviceMemory allocate(VkMemoryRequirements req,VkMemoryPropertyFlags flags) {
    VkMemoryAllocateInfo ai={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,req.size,memory_type(req.memoryTypeBits,flags)};
    VkDeviceMemory mem;CHECK(vkAllocateMemory(V.dev,&ai,NULL,&mem));return mem;
}
// Scratch allocations stay alive until the submission has completed. Poison each
// allocation first so missing rows/blocks cannot accidentally look like a clear.
static struct {VkBuffer buffer;VkDeviceMemory memory;void *mapped;} scratch[1024];
static unsigned nscratch;
static void *vk_scratch(VkDeviceSize size,VkDeviceSize alignment,VkBuffer *buffer,VkDeviceSize *offset) {
    if(nscratch==1024 || !size || !alignment)abort();
    VkBufferCreateInfo ci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,NULL,0,size,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(V.dev,&ci,NULL,buffer));
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(V.dev,*buffer,&req);
    VkDeviceMemory memory=allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkBindBufferMemory(V.dev,*buffer,memory,0));void *mapped;CHECK(vkMapMemory(V.dev,memory,0,size,0,&mapped));
    memset(mapped,0xcd,(size_t)size);*offset=0;
    scratch[nscratch].buffer=*buffer;scratch[nscratch].memory=memory;scratch[nscratch++].mapped=mapped;return mapped;
}
static void release_scratch(void) {
    for(unsigned i=0;i<nscratch;i++) {
        vkUnmapMemory(V.dev,scratch[i].memory);vkDestroyBuffer(V.dev,scratch[i].buffer,NULL);vkFreeMemory(V.dev,scratch[i].memory,NULL);
    }
    nscratch=0;
}
static void run_case(int kind,unsigned mask,int shape,int scenario) {
    int volume=shape==2;unsigned layers=shape==1 ? 6 : 4, pixel_bytes=kind<3 ? 16 : 4;
    image=(vkimg_t){.w=8,.h=8,.d=volume?8:1,.layers=volume?1:layers,.levels=3,.samples=1,.nvn_format=kind,
        .target=volume?NVN_TEXTURE_TARGET_3D:4,.fmt=formats[kind].vk,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,
        .usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkImageCreateInfo ii={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,NULL,
        volume?VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT:shape==1?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0,
        volume?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D,image.fmt,{8,8,image.d},3,image.layers,1,VK_IMAGE_TILING_OPTIMAL,
        image.usage,VK_SHARING_MODE_EXCLUSIVE,0,NULL,VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(V.dev,&ii,NULL,&image.img));
    VkMemoryRequirements req;vkGetImageMemoryRequirements(V.dev,image.img,&req);
    VkDeviceMemory imem=allocate(req,0);CHECK(vkBindImageMemory(V.dev,image.img,imem,0));
    VkBufferImageCopy copies[3];VkDeviceSize bytes=0;
    for(unsigned lv=0;lv<3;lv++) {
        copies[lv]=(VkBufferImageCopy){bytes,0,0,{VK_IMAGE_ASPECT_COLOR_BIT,lv,0,image.layers},{0,0,0},{8>>lv,8>>lv,volume?8>>lv:1}};
        bytes+=(8>>lv)*(8>>lv)*(volume?8>>lv:layers)*pixel_bytes;
    }
    VkBufferCreateInfo bi={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,NULL,0,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer;CHECK(vkCreateBuffer(V.dev,&bi,NULL,&buffer));vkGetBufferMemoryRequirements(V.dev,buffer,&req);
    VkDeviceMemory bmem=allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkBindBufferMemory(V.dev,buffer,bmem,0));
    VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(command,&begin));
    VkImageMemoryBarrier transition={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,NULL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,image.img,{VK_IMAGE_ASPECT_COLOR_BIT,0,3,0,image.layers}};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&transition);
    VkClearColorValue seed;
    for(unsigned c=0;c<4;c++) {
        if(kind==1)seed.uint32[c]=101+c;
        else if(kind==2)seed.int32[c]=-101-(int)c;
        else seed.float32[c]=(float)(c&1);
    }
    vkCmdClearColorImage(command,image.img,VK_IMAGE_LAYOUT_GENERAL,&seed,1,&transition.subresourceRange);
    vk_img_barrier_all(command);
    tex_t t={.d={.format=kind}};
    p_clear_tex clear={.tex=&t,.has_view=1,.view={.set_levels=1,.base_level=1,.nlevels=1,.set_layers=1,.min_layer=volume?0:1,.nlayers=2},
        .r={1,1,volume?1:0,2,2,1},.mask=(int)mask,.is_uint=1,.c.u={1,0,1,0}};
    // Interior, clipped top-left, whole mip, and two consecutive slices.
    if(scenario==1){clear.r.xoffset=-1;clear.r.yoffset=-1;clear.r.width=3;clear.r.height=3;}
    if(scenario==2){clear.r.xoffset=0;clear.r.yoffset=0;clear.r.width=4;clear.r.height=4;}
    if(scenario==3)clear.r.depth=2;
    VkClearColorValue replacement;
    if(kind<3) {
        const uint32_t bits[4]={17,23,42,99};memcpy(clear.c.u,bits,sizeof bits);
        if(kind==2){clear.c.u[0]=(uint32_t)-17;clear.c.u[2]=(uint32_t)-42;}
        if(scenario==1) {
            clear.is_uint=0;clear.c.f[0]=-17.75f;clear.c.f[1]=23.5f;clear.c.f[2]=42.25f;clear.c.f[3]=-99.0f;
            if(kind==0)memcpy(replacement.float32,clear.c.f,16);
            else if(kind==1){replacement.uint32[0]=0;replacement.uint32[1]=23;replacement.uint32[2]=42;replacement.uint32[3]=0;}
            else{replacement.int32[0]=-17;replacement.int32[1]=23;replacement.int32[2]=42;replacement.int32[3]=-99;}
        } else if(kind==0)for(unsigned c=0;c<4;c++)replacement.float32[c]=(float)bits[c];
        else memcpy(replacement.uint32,clear.c.u,16);
    }
    clear_tex(&clear);
    vk_img_barrier_all(command);
    vkCmdCopyImageToBuffer(command,image.img,VK_IMAGE_LAYOUT_GENERAL,buffer,3,copies);
    VkMemoryBarrier host={VK_STRUCTURE_TYPE_MEMORY_BARRIER,NULL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO,NULL,0,NULL,NULL,1,&command};
    CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
    unsigned char *data;CHECK(vkMapMemory(V.dev,bmem,0,bytes,0,(void**)&data));
    for(unsigned lv=0;lv<3;lv++)for(unsigned ly=0;ly<(volume?8>>lv:layers);ly++)
    for(unsigned y=0;y<(8>>lv);y++)for(unsigned x=0;x<(8>>lv);x++)for(unsigned c=0;c<4;c++) {
        int hit=lv==1 && ly>=1 && ly<(scenario==3?3u:2u) &&
            (int)x>=clear.r.xoffset && (int)x<clear.r.xoffset+clear.r.width &&
            (int)y>=clear.r.yoffset && (int)y<clear.r.yoffset+clear.r.height && (mask&(1u<<c));
        uint32_t expected=kind<3 ? (hit?replacement.uint32[c]:seed.uint32[c]) : (hit?!(c&1):(c&1))*255;
        size_t offset=copies[lv].bufferOffset+((ly*(8>>lv)+y)*(8>>lv)+x)*pixel_bytes;
        uint32_t actual=0;if(kind<3)memcpy(&actual,data+offset+c*4,4);else actual=data[offset+c];
        if(actual!=expected){fprintf(stderr,"Mismatch mode=%s format=%d shape=%d scenario=%d mask=%u mip=%u layer=%u xy=%u,%u channel=%u actual=%#x want=%#x\n",legacy?"legacy":"dynamic",kind,shape,scenario,mask,lv,ly,x,y,c,actual,expected);exit(1);}
    }
    vkUnmapMemory(V.dev,bmem);
    for(unsigned i=0;i<nfbs;i++)vkDestroyFramebuffer(V.dev,framebuffers[i],NULL);nfbs=0;
    for(unsigned i=0;i<nviews;i++)vkDestroyImageView(V.dev,views[i],NULL);nviews=0;
    vkDestroyBuffer(V.dev,buffer,NULL);vkFreeMemory(V.dev,bmem,NULL);
    vkDestroyImage(V.dev,image.img,NULL);vkFreeMemory(V.dev,imem,NULL);
    CHECK(vkResetCommandBuffer(command,0));
}
typedef struct {VkBuffer buffer;VkDeviceMemory memory;void *mapped;VkDeviceSize size;} host_buffer;
static host_buffer make_host_buffer(VkDeviceSize bytes,VkBufferUsageFlags usage) {
    host_buffer b={.size=bytes};
    VkBufferCreateInfo ci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,NULL,0,bytes,usage,VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateBuffer(V.dev,&ci,NULL,&b.buffer));VkMemoryRequirements req;vkGetBufferMemoryRequirements(V.dev,b.buffer,&req);
    b.memory=allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkBindBufferMemory(V.dev,b.buffer,b.memory,0));CHECK(vkMapMemory(V.dev,b.memory,0,bytes,0,&b.mapped));return b;
}
static void free_host_buffer(host_buffer b) {
    vkUnmapMemory(V.dev,b.memory);vkDestroyBuffer(V.dev,b.buffer,NULL);vkFreeMemory(V.dev,b.memory,NULL);
}
static VkDescriptorSetLayout sample_set_layout;
static VkPipelineLayout sample_layout;
static VkPipeline sample_pipeline[2];
static VkSampler sample_sampler;
static VkDescriptorPool sample_pool;
static VkDescriptorSet sample_set;
static void init_sampling(void) {
    VkDescriptorSetLayoutBinding bindings[2]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo li={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,NULL,0,2,bindings};
    CHECK(vkCreateDescriptorSetLayout(V.dev,&li,NULL,&sample_set_layout));
    VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,5*sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo pi={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,NULL,0,1,&sample_set_layout,1,&push};
    CHECK(vkCreatePipelineLayout(V.dev,&pi,NULL,&sample_layout));
    const uint32_t *code[2]={clear_sample_array_comp_spv,clear_sample_volume_comp_spv};
    const size_t sizes[2]={sizeof clear_sample_array_comp_spv,sizeof clear_sample_volume_comp_spv};
    for(unsigned i=0;i<2;i++) {
        VkShaderModuleCreateInfo mi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,sizes[i],code[i]};VkShaderModule module;
        CHECK(vkCreateShaderModule(V.dev,&mi,NULL,&module));
        VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",NULL},.layout=sample_layout};
        CHECK(vkCreateComputePipelines(V.dev,VK_NULL_HANDLE,1,&ci,NULL,&sample_pipeline[i]));vkDestroyShaderModule(V.dev,module,NULL);
    }
    VkSamplerCreateInfo si={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=16};
    CHECK(vkCreateSampler(V.dev,&si,NULL,&sample_sampler));
    VkDescriptorPoolSize ps[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
    VkDescriptorPoolCreateInfo pc={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,NULL,0,1,2,ps};
    CHECK(vkCreateDescriptorPool(V.dev,&pc,NULL,&sample_pool));
    VkDescriptorSetAllocateInfo sa={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,NULL,sample_pool,1,&sample_set_layout};
    CHECK(vkAllocateDescriptorSets(V.dev,&sa,&sample_set));
}
static void destroy_sampling(void) {
    vkDestroyDescriptorPool(V.dev,sample_pool,NULL);vkDestroySampler(V.dev,sample_sampler,NULL);
    for(unsigned i=0;i<2;i++)vkDestroyPipeline(V.dev,sample_pipeline[i],NULL);
    vkDestroyPipelineLayout(V.dev,sample_layout,NULL);vkDestroyDescriptorSetLayout(V.dev,sample_set_layout,NULL);
}
static unsigned mip_size(unsigned size,unsigned level) {return size>>level ? size>>level : 1;}
static int compressed_supported(unsigned kind,int shape) {
    const vkfmt_t *format=&formats[kind];VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(V.pd,format->vk,&fp);
    VkFormatFeatureFlags need=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    VkImageFormatProperties ip;
    VkResult support_result=vkGetPhysicalDeviceImageFormatProperties(V.pd,format->vk,shape==2?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
        shape==1?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0,&ip);
    if(support_result==VK_ERROR_FORMAT_NOT_SUPPORTED || (fp.optimalTilingFeatures&need)!=need) {
        printf("SKIP: compressed %s shape=%s unsupported for sampled transfer image\n",format_names[kind],shape==2?"3D":shape==1?"cube":"array");return 0;
    }
    CHECK(support_result);return 1;
}
static void compressed_expected(unsigned kind,const float source[4],float expected[4],float tolerance[4]) {
    VkFormat f=formats[kind].vk;
    int bc1=f>=VK_FORMAT_BC1_RGB_UNORM_BLOCK && f<=VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    int rgb=f==VK_FORMAT_BC1_RGB_UNORM_BLOCK || f==VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    int bc4=f==VK_FORMAT_BC4_UNORM_BLOCK || f==VK_FORMAT_BC4_SNORM_BLOCK;
    int bc5=f==VK_FORMAT_BC5_UNORM_BLOCK || f==VK_FORMAT_BC5_SNORM_BLOCK;
    int bc6=f==VK_FORMAT_BC6H_UFLOAT_BLOCK || f==VK_FORMAT_BC6H_SFLOAT_BLOCK;
    int signed_fmt=f==VK_FORMAT_BC4_SNORM_BLOCK || f==VK_FORMAT_BC5_SNORM_BLOCK || f==VK_FORMAT_BC6H_SFLOAT_BLOCK;
    for(unsigned c=0;c<4;c++) {
        float lo=signed_fmt ? -1.f : 0.f;
        expected[c]=bc6 && c<3 ? fmaxf(f==VK_FORMAT_BC6H_SFLOAT_BLOCK?-65504.f:0.f,fminf(65504.f,source[c])) : fmaxf(lo,fminf(1.f,source[c]));
        tolerance[c]=bc6?0.02f*fmaxf(1.f,fabsf(expected[c])):0.01f;
        if(f>=VK_FORMAT_BC1_RGB_UNORM_BLOCK && f<=VK_FORMAT_BC3_SRGB_BLOCK)tolerance[c]=0.04f;
        if(f==VK_FORMAT_BC7_UNORM_BLOCK || f==VK_FORMAT_BC7_SRGB_BLOCK)tolerance[c]=0.018f;
    }
    if(bc1)expected[3]=rgb || source[3]>=0.5f ? 1.f : 0.f;
    if(bc1 && !rgb && expected[3]==0)expected[0]=expected[1]=expected[2]=0;
    if(bc4)expected[1]=expected[2]=0;
    if(bc5)expected[2]=0;
    if(bc4 || bc5 || bc6)expected[3]=1;
}
static uint64_t sampled_pixels,readback_blocks;
static int astc_bytes_only,astc_seed_control;
static void run_compressed_case(unsigned kind,int shape,int scenario) {
    const vkfmt_t *format=&formats[kind];int volume=shape==2;
    int sample_color=!(astc_bytes_only && format->vk>=VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
    unsigned width=astc_seed_control?16:scenario==7?517:13,height=astc_seed_control?16:shape==1?13:scenario==7?259:9,depth=volume?7:1,layers=volume?1:shape==1?6:4;
    image=(vkimg_t){.w=width,.h=height,.d=depth,.layers=layers,.levels=4,.samples=1,.nvn_format=(int)kind,
        .target=volume?NVN_TEXTURE_TARGET_3D:4,.fmt=format->vk,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,
        .usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageCreateInfo ii={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,NULL,shape==1?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0,
        volume?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D,image.fmt,{width,height,depth},4,layers,1,VK_IMAGE_TILING_OPTIMAL,
        image.usage,VK_SHARING_MODE_EXCLUSIVE,0,NULL,VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(V.dev,&ii,NULL,&image.img));VkMemoryRequirements req;vkGetImageMemoryRequirements(V.dev,image.img,&req);
    VkDeviceMemory imem=allocate(req,0);CHECK(vkBindImageMemory(V.dev,image.img,imem,0));
    VkBufferImageCopy copies[4];VkDeviceSize bytes=astc_seed_control?0:16;uint32_t texels=0,sample_offsets[4];
    for(unsigned lv=0;lv<4;lv++) {
        unsigned w=mip_size(width,lv),h=mip_size(height,lv),d=mip_size(depth,lv);
        copies[lv]=(VkBufferImageCopy){bytes,0,0,{VK_IMAGE_ASPECT_COLOR_BIT,lv,0,layers},{0,0,0},{w,h,volume?d:1}};
        bytes+=(VkDeviceSize)((w+format->bw-1)/format->bw)*((h+format->bh-1)/format->bh)*(volume?d:layers)*format->bpb;
        sample_offsets[lv]=texels;texels+=w*h*(volume?d:layers);
    }
    host_buffer upload=make_host_buffer(bytes+16,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    host_buffer raw=make_host_buffer(bytes+16,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    host_buffer sampled=make_host_buffer((VkDeviceSize)texels*16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memset(raw.mapped,0xa5,(size_t)raw.size);memset(sampled.mapped,0xff,(size_t)sampled.size);
    float seed[4]={0.08f,0.24f,0.71f,1.f},color[4]={0.72f,0.37f,0.13f,0.6f},second[4]={0.27f,0.81f,0.46f,1.f};
    if(format->vk==VK_FORMAT_BC4_SNORM_BLOCK || format->vk==VK_FORMAT_BC5_SNORM_BLOCK || format->vk==VK_FORMAT_BC6H_SFLOAT_BLOCK){seed[0]=-0.24f;color[0]=-0.72f;}
    if(format->vk==VK_FORMAT_BC6H_UFLOAT_BLOCK || format->vk==VK_FORMAT_BC6H_SFLOAT_BLOCK){seed[2]=3.5f;color[1]=7.25f;second[2]=16.f;}
    if(scenario==3 && format->vk>=VK_FORMAT_BC1_RGBA_UNORM_BLOCK && format->vk<=VK_FORMAT_BC1_RGBA_SRGB_BLOCK)color[3]=0;
    if(scenario==6){color[0]=1;color[1]=0;color[2]=1;color[3]=1;}
    uint8_t seed_block[16],color_block[16],second_block[16];
    if(astc_seed_control) {
        // ARM astcenc UNORM16 void-extent reference: RGBA=(.08,.24,.71,1).
        // No production encoder or clear is called in this baseline control.
        const uint8_t reference[16]={0xfc,0xfd,0xff,0xff,0xff,0xff,0xff,0xff,0x7b,0x14,0x70,0x3d,0xc2,0xb5,0xff,0xff};
        memcpy(seed_block,reference,16);memset(color_block,0,16);memset(second_block,0,16);
    } else if(vk_compressed_clear_block(format->vk,seed,seed_block)!=format->bpb || vk_compressed_clear_block(format->vk,color,color_block)!=format->bpb || vk_compressed_clear_block(format->vk,second,second_block)!=format->bpb)abort();
    for(VkDeviceSize off=astc_seed_control?0:16;off<bytes;off+=format->bpb)memcpy((char*)upload.mapped+off,seed_block,format->bpb);
    VkImageViewCreateInfo vi={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,NULL,0,image.img,volume?VK_IMAGE_VIEW_TYPE_3D:VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        image.fmt,{0,0,0,0},{VK_IMAGE_ASPECT_COLOR_BIT,0,4,0,layers}};VkImageView view;CHECK(vkCreateImageView(V.dev,&vi,NULL,&view));
    VkDescriptorImageInfo idesc={sample_sampler,view,VK_IMAGE_LAYOUT_GENERAL};VkDescriptorBufferInfo bdesc={sampled.buffer,0,sampled.size};
    VkWriteDescriptorSet writes[2]={{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,NULL,sample_set,0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&idesc,NULL,NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,NULL,sample_set,1,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,NULL,&bdesc,NULL}};
    vkUpdateDescriptorSets(V.dev,2,writes,0,NULL);
    VkCommandBufferBeginInfo begin={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(command,&begin));
    VkImageMemoryBarrier transition={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,NULL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,image.img,{VK_IMAGE_ASPECT_COLOR_BIT,0,4,0,layers}};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&transition);
    vkCmdCopyBufferToImage(command,upload.buffer,image.img,VK_IMAGE_LAYOUT_GENERAL,4,copies);vk_img_barrier_all(command);
    unsigned level=scenario==1 || scenario==4?1:scenario==2?3:0;
    unsigned w=mip_size(width,level),h=mip_size(height,level),d=mip_size(depth,level);
    tex_t t={.d={.format=(int)kind}};
    p_clear_tex clear={.tex=&t,.has_view=1,.view={.set_levels=1,.base_level=(int)level,.nlevels=1,.set_layers=1,.min_layer=volume?0:1,.nlayers=volume?1:2},
        .r={0,0,0,(int)w,(int)h,volume?(int)d:2},.mask=scenario==5?0:scenario==4?5:15};
    memcpy(clear.c.f,color,16);
    if(scenario==6){clear.is_uint=1;for(unsigned c=0;c<4;c++)clear.c.u[c]=(uint32_t)color[c];}
    if(scenario==3){clear.r.xoffset=1;clear.r.yoffset=1;clear.r.width=3;clear.r.height=2;}
    if(volume && (scenario==3 || scenario==4)){clear.r.zoffset=1;clear.r.depth=1;}
    fallback_diagnostics=0;allow_compressed_fallback=1;if(!astc_seed_control)clear_tex(&clear);allow_compressed_fallback=0;
    int needs_fallback=scenario==3 || scenario==4;
    if((fallback_diagnostics!=0)!=needs_fallback || image.gpu_w!=(scenario!=5)) {
        fprintf(stderr,"Compressed clear routing mismatch %s shape=%d scenario=%d diagnostic=%d gpu_w=%d\n",format_names[kind],shape,scenario,fallback_diagnostics,image.gpu_w);exit(1);
    }
    // A second pending clear with another color verifies submission-owned upload
    // data remains alive and distinct until both commands execute on the GPU.
    p_clear_tex follow={.tex=&t,.has_view=1,.view={.set_levels=1,.base_level=2,.nlevels=1,.set_layers=1,.min_layer=0,.nlayers=1},
        .r={0,0,0,(int)mip_size(width,2),(int)mip_size(height,2),volume?(int)mip_size(depth,2):1},.mask=15};
    memcpy(follow.c.f,second,16);if(!astc_seed_control)clear_tex(&follow);vk_img_barrier_all(command);
    vkCmdCopyImageToBuffer(command,image.img,VK_IMAGE_LAYOUT_GENERAL,raw.buffer,4,copies);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,sample_pipeline[volume]);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,sample_layout,0,1,&sample_set,0,NULL);
    for(unsigned lv=0;sample_color && lv<4;lv++) {
        uint32_t push[5]={mip_size(width,lv),mip_size(height,lv),volume?mip_size(depth,lv):layers,lv,sample_offsets[lv]};
        vkCmdPushConstants(command,sample_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,push);
        vkCmdDispatch(command,(push[0]+7)/8,(push[1]+7)/8,push[2]);
    }
    VkMemoryBarrier host={VK_STRUCTURE_TYPE_MEMORY_BARRIER,NULL,VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(command));VkSubmitInfo submit={VK_STRUCTURE_TYPE_SUBMIT_INFO,NULL,0,NULL,NULL,1,&command};
    CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
    for(unsigned lv=0;lv<4;lv++) {
        unsigned mw=mip_size(width,lv),mh=mip_size(height,lv),nz=volume?mip_size(depth,lv):layers;
        unsigned bx=(mw+format->bw-1)/format->bw,by=(mh+format->bh-1)/format->bh;
        for(unsigned z=0;z<nz;z++) {
            int hit=scenario!=5 && lv==level && (volume || (z>=1 && z<3));
            int second_hit=!astc_seed_control && lv==2 && (volume || z==0);
            const uint8_t *block=second_hit?second_block:hit?color_block:seed_block;
            for(unsigned b=0;b<bx*by;b++) {
                VkDeviceSize off=copies[lv].bufferOffset+((VkDeviceSize)z*bx*by+b)*format->bpb;readback_blocks++;
                if(memcmp((char*)raw.mapped+off,block,format->bpb)) {
                    fprintf(stderr,"Compressed bytes mismatch %s shape=%d scenario=%d mip=%u layer=%u block=%u\n",format_names[kind],shape,scenario,lv,z,b);exit(1);
                }
            }
            float expected[4],tolerance[4];compressed_expected(kind,second_hit?second:hit?color:seed,expected,tolerance);
            for(unsigned y=0;sample_color && y<mh;y++)for(unsigned x=0;x<mw;x++) {
                const float *actual=(float*)sampled.mapped+4*(sample_offsets[lv]+(z*mh+y)*mw+x);sampled_pixels++;
                for(unsigned c=0;c<4;c++)if(!isfinite(actual[c]) || fabsf(actual[c]-expected[c])>tolerance[c]) {
                    fprintf(stderr,"Sampled RGBA=(%g,%g,%g,%g) expected=(%g,%g,%g,%g)\n",actual[0],actual[1],actual[2],actual[3],expected[0],expected[1],expected[2],expected[3]);
                    fprintf(stderr,"Sampled color mismatch %s shape=%d scenario=%d mip=%u layer=%u xy=%u,%u channel=%u actual=%g expected=%g tolerance=%g\n",format_names[kind],shape,scenario,lv,z,x,y,c,actual[c],expected[c],tolerance[c]);exit(1);
                }
            }
        }
    }
    for(unsigned i=0;i<16;i++)if((!astc_seed_control && ((unsigned char*)raw.mapped)[i]!=0xa5) || ((unsigned char*)raw.mapped)[bytes+i]!=0xa5)abort();
    release_scratch();vkDestroyImageView(V.dev,view,NULL);free_host_buffer(upload);free_host_buffer(raw);free_host_buffer(sampled);
    vkDestroyImage(V.dev,image.img,NULL);vkFreeMemory(V.dev,imem,NULL);CHECK(vkResetCommandBuffer(command,0));
}
static void run_compressed(void) {
    init_sampling();unsigned cases=0,supported=0,skips=0,sampling_skips=0;
    if(astc_seed_control) {
        printf("ASTC seed control: hard-coded fcfdffffffffffff7b14703dc2b5ffff, 16x16, buffer offset 0, no production encoder/clear\n");
        if(compressed_supported(21,0)) {
            run_compressed_case(21,0,5);
            puts("PASS: ASTC reference upload raw bytes and GPU-decoded color");
        }
        destroy_sampling();return;
    }
    if(astc_bytes_only)puts("SKIP: ASTC decoded-color sampling explicitly disabled by --astc-bytes-only; ASTC raw bytes and mip/layer preservation remain checked");
    for(unsigned kind=5;kind<sizeof formats/sizeof *formats;kind++)for(int shape=0;shape<3;shape++) {
        if(!compressed_supported(kind,shape)){skips++;continue;}
        supported++;
        for(int scenario=0;scenario<7;scenario++){run_compressed_case(kind,shape,scenario);cases++;if(astc_bytes_only && formats[kind].vk>=VK_FORMAT_ASTC_4x4_UNORM_BLOCK)sampling_skips++;}
        if(shape==0 && (formats[kind].vk==VK_FORMAT_BC1_RGBA_UNORM_BLOCK || formats[kind].vk==VK_FORMAT_BC7_UNORM_BLOCK || formats[kind].vk==VK_FORMAT_ASTC_6x5_UNORM_BLOCK)) {
            run_compressed_case(kind,shape,7);cases++;if(astc_bytes_only && formats[kind].vk>=VK_FORMAT_ASTC_4x4_UNORM_BLOCK)sampling_skips++;
        }
        printf("PASS: compressed %s shape=%s full, partial, masked, zero-mask, tail-mip, uint-entry and pending-clear preservation; %s\n",format_names[kind],shape==2?"3D":shape==1?"cube":"array",astc_bytes_only && formats[kind].vk>=VK_FORMAT_ASTC_4x4_UNORM_BLOCK?"raw bytes only; decoded sampling SKIPPED":"GPU-decoded colors checked");
    }
    printf("PASS: %u compressed clear/readback cases, %u supported format/shapes, %u unsupported format/shape skips, %u ASTC sampling cases explicitly skipped; %llu sampled pixels, %llu raw blocks checked\n",cases,supported,skips,sampling_skips,(unsigned long long)sampled_pixels,(unsigned long long)readback_blocks);
    destroy_sampling();
}

int main(int argc,char **argv) {
    setbuf(stdout,NULL);
    legacy=argc>1 && !strcmp(argv[1],"legacy");
    int compressed_only=argc>2 && !strcmp(argv[2],"compressed");
    astc_bytes_only=argc>3 && !strcmp(argv[3],"astc-bytes-only");
    astc_seed_control=argc>4 && !strcmp(argv[4],"astc-seed-control");
    if(astc_seed_control){compressed_only=1;astc_bytes_only=0;}
    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO,NULL,"texture-clear",1,NULL,0,VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,NULL,0,&app};VkInstance instance;CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=1;CHECK(vkEnumeratePhysicalDevices(instance,&count,&V.pd));if(!count)return 1;
    VkPhysicalDeviceProperties prop;vkGetPhysicalDeviceProperties(V.pd,&prop);printf("%s: %s\n",legacy?"legacy":"dynamic",prop.deviceName);
    VkPhysicalDeviceDynamicRenderingFeatures dynamic={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceFeatures2 features={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&dynamic};vkGetPhysicalDeviceFeatures2(V.pd,&features);
    printf("Vulkan api=%#x vendor=%#x device=%#x driver=%#x BC=%u ASTC_LDR=%u\n",prop.apiVersion,prop.vendorID,prop.deviceID,prop.driverVersion,features.features.textureCompressionBC,features.features.textureCompressionASTC_LDR);
    V.has_dynrender=dynamic.dynamicRendering;if(!legacy && !V.has_dynrender){fprintf(stderr,"Dynamic rendering unavailable\n");return 1;}
    uint32_t nq=0;vkGetPhysicalDeviceQueueFamilyProperties(V.pd,&nq,NULL);VkQueueFamilyProperties *qs=calloc(nq,sizeof *qs);vkGetPhysicalDeviceQueueFamilyProperties(V.pd,&nq,qs);
    uint32_t qf=0;while(qf<nq && (qs[qf].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))!=(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))qf++;free(qs);if(qf==nq)return 1;
    float priority=1;VkDeviceQueueCreateInfo qi={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,NULL,0,qf,1,&priority};
    VkPhysicalDeviceFeatures enabled={.textureCompressionBC=features.features.textureCompressionBC,.textureCompressionASTC_LDR=features.features.textureCompressionASTC_LDR};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&dynamic,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.pEnabledFeatures=&enabled};CHECK(vkCreateDevice(V.pd,&di,NULL,&V.dev));vkGetDeviceQueue(V.dev,qf,0,&queue);
    begin_rendering=(PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(V.dev,"vkCmdBeginRendering");end_rendering=(PFN_vkCmdEndRendering)vkGetDeviceProcAddr(V.dev,"vkCmdEndRendering");
    VkCommandPoolCreateInfo pi={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,NULL,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,qf};VkCommandPool pool;CHECK(vkCreateCommandPool(V.dev,&pi,NULL,&pool));
    VkCommandBufferAllocateInfo ai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,NULL,pool,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};CHECK(vkAllocateCommandBuffers(V.dev,&ai,&command));
    unsigned tests=0;for(int kind=0;!compressed_only && kind<5;kind++)for(int shape=0;shape<3;shape++)for(int scenario=0;scenario<4;scenario++)for(unsigned mask=0;mask<16;mask++){run_case(kind,mask,shape,scenario);tests++;}
    if(!compressed_only)printf("PASS: %u clear/readback cases; untouched channels, pixels, layers and mips preserved\n",tests);
    run_compressed();
    for(unsigned i=0;i<g_mc_pipes.cap;i++)if(g_mc_pipes.vals[i] && g_mc_pipes.vals[i]!=(void *)(uintptr_t)1)vkDestroyPipeline(V.dev,g_mc_pipes.vals[i],NULL);
    free(g_mc_pipes.keys);free(g_mc_pipes.vals);
    for(unsigned i=0;i<3;i++)vkDestroyShaderModule(V.dev,g_mc.fs[i],NULL);vkDestroyShaderModule(V.dev,g_mc.vs,NULL);vkDestroyPipelineLayout(V.dev,g_mc.pl,NULL);
    for(unsigned i=0;i<5;i++)if(passes[i])vkDestroyRenderPass(V.dev,passes[i],NULL);
    vkDestroyCommandPool(V.dev,pool,NULL);vkDestroyDevice(V.dev,NULL);vkDestroyInstance(instance,NULL);return 0;
}
