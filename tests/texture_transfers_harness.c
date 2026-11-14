#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nxnative/dm/gfx/nvn/nvn_obj.h"
/* TYPES */
#define VKCHK(x) assert((x)==VK_SUCCESS)
static int red_calls;
static int fail_alloc, dedicated, direct_allocs, pool_calls, pooled_retired, direct_retired, destroyed;
static VkDeviceSize bound_offset;
static int slot_cookie, create_calls, fail_n;
#define VKRED_ONCE(...) ((void)red_calls++)
static struct { VkDevice dev; VkPhysicalDevice pd; } V;
static uint64_t g_tex_frame;
static tex_t src, dst;
static vkimg_t images[16];
static int nimages, copies, blits, resolves, retired, barriers, same_image;
static VkImageCopy last_copy;
static VkImageResolve last_resolve;
static VkImageBlit last_blit;
static VkClearColorValue last_clear;
static VkImage last_copy_src, last_copy_dst, last_resolve_src, last_resolve_dst;
static VkFormat blit_src_fmt, blit_dst_fmt;
static vkimg_t *by_handle(VkImage h) { uintptr_t n=(uintptr_t)h; assert(n>0 && n<=(uintptr_t)nimages); return &images[n-1]; }
static const vkfmt_t *vk_fmt(int n) {
    static const vkfmt_t f[]={{.vk=VK_FORMAT_R8G8B8A8_UNORM},{.vk=VK_FORMAT_R8G8B8A8_SRGB}, {.vk=VK_FORMAT_B8G8R8A8_UNORM},{.vk=VK_FORMAT_B8G8R8A8_SRGB},{.vk=VK_FORMAT_D32_SFLOAT},{.vk=VK_FORMAT_D16_UNORM}};
    switch(n) {
    case NVN_FORMAT_RGBA8: case NVN_FORMAT_RGBX8:return &f[0];
    case NVN_FORMAT_RGBA8_SRGB:case NVN_FORMAT_RGBX8_SRGB:return &f[1];
    case NVN_FORMAT_BGRA8:case NVN_FORMAT_BGRX8:return &f[2];
    case NVN_FORMAT_BGRA8_SRGB:case NVN_FORMAT_BGRX8_SRGB:return &f[3];
    case NVN_FORMAT_DEPTH32F:return &f[4];
    case NVN_FORMAT_DEPTH16:return &f[5];
    default:abort();
    }
}
static vkimg_t *vk_tex(const tex_t *t) { return &images[same_image || t==&src?0:1]; }
static void vk_end_pass(void) {}
static void vk_watch(const vkimg_t *im,const char *why) {(void)im;(void)why;}
static VkCommandBuffer vk_cmd(void) {return (VkCommandBuffer)(uintptr_t)1;}
static void vk_tex_prepare(VkCommandBuffer cb,vkimg_t *im) {(void)cb;(void)im;}
static void vk_img_barrier_all(VkCommandBuffer cb) {(void)cb;barriers++;}
static uint32_t vk_mem_type(uint32_t bits,VkMemoryPropertyFlags want,VkMemoryPropertyFlags avoid) {(void)bits;(void)want;(void)avoid;return 0;}
static void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p,VkFormat f,VkFormatProperties *o) {
    (void)p;(void)f;*o=(VkFormatProperties){.optimalTilingFeatures=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT};
}
static VkResult vkCreateImage(VkDevice dev,const VkImageCreateInfo *ci,const VkAllocationCallbacks *a,VkImage *out) {
    (void)dev;(void)a; if(++create_calls==fail_n && fail_alloc==1)return VK_ERROR_OUT_OF_DEVICE_MEMORY;assert(nimages<16);assert(ci->samples==VK_SAMPLE_COUNT_1_BIT);
    assert(ci->usage==(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT));
    assert(ci->imageType==VK_IMAGE_TYPE_2D && ci->arrayLayers==1 && ci->mipLevels==1);
    *out=(VkImage)(uintptr_t)(nimages+1); images[nimages++]=(vkimg_t){.img=*out,.fmt=ci->format,.w=ci->extent.width,.h=ci->extent.height,.d=1,.layers=1,.levels=1,.samples=1};return VK_SUCCESS;
}
static void vkGetImageMemoryRequirements(VkDevice d,VkImage im,VkMemoryRequirements *r) {(void)d;(void)im;*r=(VkMemoryRequirements){.size=4096,.memoryTypeBits=1};}
static VkResult vkAllocateMemory(VkDevice d,const VkMemoryAllocateInfo *a,const VkAllocationCallbacks *p,VkDeviceMemory *out) {(void)d;(void)a;(void)p;direct_allocs++;if(fail_alloc==2 && create_calls==fail_n)return VK_ERROR_OUT_OF_DEVICE_MEMORY;*out=(VkDeviceMemory)(uintptr_t)1;return VK_SUCCESS;}
static VkResult vkBindImageMemory(VkDevice d,VkImage i,VkDeviceMemory m,VkDeviceSize off) {(void)d;(void)i;(void)m;bound_offset=off;return fail_alloc==3 && create_calls==fail_n?VK_ERROR_OUT_OF_DEVICE_MEMORY:VK_SUCCESS;}
static void vkCmdPipelineBarrier(VkCommandBuffer cb,VkPipelineStageFlags srcstage,VkPipelineStageFlags dststage,VkDependencyFlags flags,uint32_t nm,const VkMemoryBarrier *mb,uint32_t nb,const VkBufferMemoryBarrier *bb,uint32_t ni,const VkImageMemoryBarrier *ib) {
    (void)cb;(void)srcstage;(void)dststage;(void)flags;(void)nm;(void)mb;(void)nb;(void)bb;assert(ni==1);assert(ib->oldLayout==VK_IMAGE_LAYOUT_UNDEFINED && ib->newLayout==VK_IMAGE_LAYOUT_GENERAL);barriers++;
}
static VkDeviceMemory vk_image_memory(const VkMemoryRequirements *r,uint32_t type,VkDeviceSize *off,void **sub) {
    (void)r;(void)type;pool_calls++;*off=0;*sub=NULL;if(fail_alloc==2 && create_calls==fail_n)return VK_NULL_HANDLE;
    if(!dedicated){*off=4096;*sub=&slot_cookie;}return (VkDeviceMemory)(uintptr_t)1;
}
static void vkDestroyImage(VkDevice dev,VkImage im,const VkAllocationCallbacks *a) {(void)dev;(void)a;assert((uintptr_t)im>2);destroyed++;}
static void vk_defer_free_image(VkImage im,VkDeviceMemory m) {assert((uintptr_t)im>2 && m);retired++;direct_retired++;}
static void vk_defer_free_image_sub(VkImage im,void *sub) {assert((uintptr_t)im>2 && sub==&slot_cookie);retired++;pooled_retired++;}
static int depth_scale(VkCommandBuffer cb,vkimg_t *s,vkimg_t *d,VkImageSubresourceLayers ss,VkImageSubresourceLayers ds,const p_copy_t2t *c) {(void)cb;(void)s;(void)d;(void)ss;(void)ds;(void)c;abort();}
static void vkCmdCopyImage(VkCommandBuffer cb,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageCopy *r) {
    (void)cb;(void)sl;(void)dl;assert(n==1);copies++;last_copy=*r;last_copy_src=s;last_copy_dst=d;
}
static void vkCmdResolveImage(VkCommandBuffer cb,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageResolve *r) {
    (void)cb;(void)sl;(void)dl;assert(n==1);assert(by_handle(s)->fmt==by_handle(d)->fmt);assert(by_handle(s)->samples>1 && by_handle(d)->samples==1);
    resolves++;last_resolve=*r;last_resolve_src=s;last_resolve_dst=d;
}
static void vkCmdBlitImage(VkCommandBuffer cb,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageBlit *r,VkFilter filter) {
    (void)cb;(void)sl;(void)dl;(void)filter;assert(n==1);blits++;last_blit=*r;blit_src_fmt=by_handle(s)->fmt;blit_dst_fmt=by_handle(d)->fmt;
}
static void vkCmdClearColorImage(VkCommandBuffer cb,VkImage im,VkImageLayout l,const VkClearColorValue *v,uint32_t n,const VkImageSubresourceRange *r) {(void)cb;(void)im;(void)l;(void)r;assert(n==1);last_clear=*v;}
static void vkCmdClearDepthStencilImage(VkCommandBuffer cb,VkImage im,VkImageLayout l,const VkClearDepthStencilValue *v,uint32_t n,const VkImageSubresourceRange *r) {(void)cb;(void)im;(void)l;(void)v;(void)n;(void)r;abort();}
/* FUNCTIONS */
static p_copy_t2t setup(int fmt,VkFormat sf,VkFormat df) {
    src=(tex_t){.d={.format=fmt}};dst=src;nimages=2;copies=blits=resolves=retired=barriers=same_image=red_calls=0;fail_alloc=dedicated=direct_allocs=pool_calls=pooled_retired=direct_retired=destroyed=0;bound_offset=0;create_calls=0;fail_n=1;
    images[0]=(vkimg_t){.img=(VkImage)(uintptr_t)1,.fmt=sf,.w=32,.h=32,.d=1,.levels=2,.layers=4,.samples=1,.aspect=VK_IMAGE_ASPECT_COLOR_BIT};
    images[1]=images[0];images[1].img=(VkImage)(uintptr_t)2;images[1].fmt=df;
    return (p_copy_t2t){.src=&src,.dst=&dst,.sr={1,2,1,4,4,1},.dr={3,4,2,4,4,1},.has_sv=1,.has_dv=1,.sv={.set_levels=1,.base_level=1},.dv={.set_levels=1,.base_level=1}};
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"memory")) for(int own=0;own<2;own++) {
        setup(NVN_FORMAT_RGBA8_SRGB,VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_R8G8B8A8_UNORM);dedicated=own;
        assert(transfer_image(vk_cmd(),VK_FORMAT_R8G8B8A8_SRGB,8,8));
        assert(pool_calls==1 && direct_allocs==0 && retired==1 && !destroyed);
        assert(bound_offset==(own?0:4096) && pooled_retired==!own && direct_retired==own);
    }
    if(!strcmp(argv[1],"memory_failure")) for(int failure=1;failure<=3;failure++) for(int path=0;path<3;path++) {
        p_copy_t2t c=setup(NVN_FORMAT_RGBA8_SRGB,path==1?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_R8G8B8A8_SRGB,path==1?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM);
        fail_alloc=failure;if(path==0){images[0].samples=4;c.flags=DM_COPY_RESOLVE;c.sv.base_level=0;}else{c.dr.width=c.dr.height=8;}
        copy_t2t(&c);assert(!copies && !blits && !resolves && red_calls==1);
        assert(destroyed==(failure==2) && retired==(failure==3));
    }
    if(!strcmp(argv[1],"memory_failure")) for(int failure=1;failure<=3;failure++) {
        p_copy_t2t c=setup(NVN_FORMAT_RGBA8_SRGB,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM);
        fail_alloc=failure;fail_n=2;c.dr.width=c.dr.height=8;copy_t2t(&c);
        // The source staging copy may exist, but a failed second temporary must
        // not blit or write the destination; both allocated images retire safely.
        assert(copies==1 && last_copy_dst!=images[1].img && !blits && !resolves && red_calls==1);
        assert(destroyed==(failure==2) && retired==1+(failure==3));
    }
    const int fmt[]={NVN_FORMAT_RGBA8_SRGB,NVN_FORMAT_RGBX8_SRGB,NVN_FORMAT_BGRA8_SRGB,NVN_FORMAT_BGRX8_SRGB};
    const VkFormat srgb[]={VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_B8G8R8A8_SRGB}, unorm[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM};
    if(!strcmp(argv[1],"resolve_invalid")) for(int invalid=0;invalid<3;invalid++) {
        p_copy_t2t c=setup(NVN_FORMAT_RGBA8_SRGB,VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_B8G8R8A8_UNORM);
        images[0].samples=4;c.flags=DM_COPY_RESOLVE;c.sv.base_level=0;
        if(invalid==0)dst.d.format=NVN_FORMAT_BGRA8_SRGB; // different logical families
        if(invalid==2) { // a depth resolve cannot use a color temporary or a format-changing raw copy
            src.d.format=NVN_FORMAT_DEPTH32F;dst.d.format=NVN_FORMAT_DEPTH16;
            images[0].fmt=VK_FORMAT_D32_SFLOAT;images[1].fmt=VK_FORMAT_D16_UNORM;
            images[0].aspect=images[1].aspect=VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        copy_t2t(&c);assert(!copies && !resolves && !blits && !retired && nimages==2 && red_calls==1);
    }
    for(int f=0;f<4;f++) {
        if(!strcmp(argv[1],"copy")) for(int reverse=0;reverse<2;reverse++) {
            p_copy_t2t c=setup(fmt[f],reverse?unorm[f/2]:srgb[f/2],reverse?srgb[f/2]:unorm[f/2]);copy_t2t(&c);
            assert(copies==1 && !blits && !resolves && !retired);assert(last_copy.srcSubresource.mipLevel==1 && last_copy.srcSubresource.baseArrayLayer==1);
            assert(last_copy.dstSubresource.mipLevel==1 && last_copy.dstSubresource.baseArrayLayer==2);assert(last_copy.extent.width==4 && last_copy.extent.height==4);
        }
        if(!strcmp(argv[1],"resolve")) for(int shared=0;shared<2;shared++) {
            p_copy_t2t c=setup(fmt[f],srgb[f/2],shared?unorm[f/2]:srgb[f/2]);images[0].samples=4;c.sv.base_level=0;c.flags=DM_COPY_RESOLVE;copy_t2t(&c);
            assert(resolves==1 && !blits && copies==shared && retired==shared);assert(last_resolve_src==images[0].img && last_resolve.srcSubresource.baseArrayLayer==1);
            if(shared) {assert(last_resolve.dstSubresource.mipLevel==0 && last_resolve.dstSubresource.baseArrayLayer==0);assert(last_copy_src==last_resolve_dst && last_copy_dst==images[1].img);assert(last_copy.dstSubresource.mipLevel==1 && last_copy.dstSubresource.baseArrayLayer==2);assert(last_copy.dstOffset.x==3 && last_copy.dstOffset.y==4);}
        }
        if(!strcmp(argv[1],"clear_view")) for(int srgb_view=0;srgb_view<2;srgb_view++) {
            setup(fmt[f],srgb[f/2],unorm[f/2]);
            int linear=f<2?NVN_FORMAT_RGBA8:NVN_FORMAT_BGRA8;
            dst.d.format=srgb_view?linear:fmt[f];
            p_clear_tex c={.tex=&dst,.r={0,0,0,32,32,1},.c.f={0.5f,0.25f,0.75f,0.25f},.has_view=1,.view={.set_format=1,.format=srgb_view?fmt[f]:linear}};
            clear_tex(&c);assert(fabsf(last_clear.float32[0]-(srgb_view?0.73535698f:0.5f))<0.00001f && last_clear.float32[3]==0.25f);
            // A dormant view format must not override the texture's format.
            c.view.set_format=0;clear_tex(&c);assert(fabsf(last_clear.float32[0]-(srgb_view?0.5f:0.73535698f))<0.00001f);
            c.view.set_format=1;c.has_view=0;clear_tex(&c);assert(fabsf(last_clear.float32[0]-(srgb_view?0.5f:0.73535698f))<0.00001f);
        }
        if(!strcmp(argv[1],"clear")) for(int shared=0;shared<2;shared++) {
            setup(fmt[f],srgb[f/2],shared?unorm[f/2]:srgb[f/2]);
            p_clear_tex c={.tex=&dst,.r={0,0,0,32,32,1},.c.f={0.5f,0.0031308f,1.0f,0.25f}};clear_tex(&c);
            assert(fabsf(last_clear.float32[0]-(shared?0.73535698f:0.5f))<0.00001f);
            assert(fabsf(last_clear.float32[1]-(shared?0.04044994f:0.0031308f))<0.00001f);
            assert(fabsf(last_clear.float32[2]-1.0f)<0.00001f && last_clear.float32[3]==0.25f);
        }
        if(!strcmp(argv[1],"blit")) for(int mirror=0;mirror<2;mirror++) {
            p_copy_t2t c=setup(fmt[f],unorm[f/2],unorm[f/2]);if(mirror)c.flags=8;else c.dr.width=c.dr.height=8;copy_t2t(&c);
            assert(blits==1 && copies==2 && retired==2);assert(blit_src_fmt==srgb[f/2] && blit_dst_fmt==srgb[f/2]);
            if(mirror)assert(last_blit.dstOffsets[0].y>last_blit.dstOffsets[1].y);
            assert(last_copy.dstSubresource.baseArrayLayer==2 && last_copy.dstSubresource.mipLevel==1);
            c=setup(fmt[f],unorm[f/2],unorm[f/2]);dst.d.format=f<2?NVN_FORMAT_RGBA8:NVN_FORMAT_BGRA8;copy_t2t(&c);
            assert(blits==1 && copies==1 && retired==1);assert(blit_src_fmt==srgb[f/2] && blit_dst_fmt==unorm[f/2]);
            // A conversion over the exact same aliased region must still execute.
            c=setup(fmt[f],unorm[f/2],unorm[f/2]);dst.d.format=f<2?NVN_FORMAT_RGBA8:NVN_FORMAT_BGRA8;
            same_image=1;c.dr=c.sr;copy_t2t(&c);assert(blits==1 && copies==1 && retired==1);
        }
    }
    puts("texture transfer command contract passed");
}
