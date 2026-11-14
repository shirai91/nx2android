// CPU oracle for the real clear dispatcher. Vulkan entry points model writes,
// not selection: the production code must choose the correct region and mask.
#include <vulkan/vulkan_core.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include "vk_compressed_clear.h"
enum { NVN_TEXTURE_TARGET_1D = 0, NVN_TEXTURE_TARGET_3D = 2, NVN_TEXTURE_TARGET_1D_ARRAY = 3 };
typedef struct { int format, target; } desc;
typedef struct { desc d; } tex_t;
typedef struct { int set_levels, base_level, nlevels, set_layers, min_layer, nlayers, set_format, format; } view_t;
typedef struct { int xoffset,yoffset,zoffset,width,height,depth; } NVNcopyRegion;
typedef struct { const tex_t *tex; view_t view; uint8_t has_view,is_uint; NVNcopyRegion r; int mask; union {float f[4];uint32_t u[4];} c; } p_clear_tex;
typedef struct { VkImage img; uint32_t w,h,d,layers,levels,samples; int target,nvn_format; VkFormat fmt; VkImageAspectFlags aspect; VkImageUsageFlags usage; int gpu_w; } vkimg_t;
typedef struct { VkFormat vk; int is_int,is_sint,compressed,dec,bw,bh,bpb; } vkfmt_t;
static vkimg_t image;
static uint32_t pixels[3][4][8][8][4];
static VkFormat vk_view_fmt;
static int diagnostics, view_level, view_layer, transfers, draws, depths, fail_view, fail_draw, wrong_format;
static VkImageSubresourceRange depth_range;
static VkClearDepthStencilValue depth_value;
static unsigned char scratch[65536+16], covered[3][8][8192];
static VkDeviceSize scratch_size;
static unsigned copies, scratch_calls;
#define VKRED_ONCE(...) ((void)++diagnostics)
static vkimg_t *vk_tex(const tex_t *t) { (void)t;return &image; }
static const vkfmt_t *vk_fmt(int f) {
    static const vkfmt_t formats[]={{VK_FORMAT_R32G32B32A32_SFLOAT,0,0,0,0},{VK_FORMAT_R32G32B32A32_UINT,1,0,0,0},{VK_FORMAT_R32G32B32A32_SINT,1,1,0,0},{VK_FORMAT_D32_SFLOAT_S8_UINT,0,0,0,0},{VK_FORMAT_R32G32B32A32_SFLOAT,0,0,1,1},{VK_FORMAT_BC1_RGBA_UNORM_BLOCK,0,0,1,0,4,4,8}};
    return f>=0 && f<6 ? &formats[f] : NULL;
}
static void vk_end_pass(void) {}
static VkCommandBuffer vk_cmd(void) {return VK_NULL_HANDLE;}
static void vk_tex_prepare(VkCommandBuffer cb,vkimg_t *im) {(void)cb;(void)im;}
static void vk_img_barrier_all(VkCommandBuffer cb) {(void)cb;}
static void *vk_scratch(VkDeviceSize n,VkDeviceSize align,VkBuffer *buf,VkDeviceSize *off) {
    assert(n>0 && n<=65536 && align>=8);scratch_calls++;scratch_size=n;
    *buf=(VkBuffer)(uintptr_t)9;*off=16;memset(scratch,0xcc,sizeof scratch);return scratch+16;
}
static void fake_copy_blocks(VkCommandBuffer cb,VkBuffer buf,VkImage im,VkImageLayout layout,uint32_t n,const VkBufferImageCopy *rs) {
    (void)cb;(void)im;assert(buf==(VkBuffer)(uintptr_t)9 && layout==VK_IMAGE_LAYOUT_GENERAL);
    for(unsigned i=0;i<n;i++) {
        const VkBufferImageCopy *r=rs+i;unsigned lv=r->imageSubresource.mipLevel;
        unsigned w=image.w>>lv ? image.w>>lv : 1,h=image.h>>lv ? image.h>>lv : 1;
        unsigned bx=(w+3)/4,rows=(r->imageExtent.height+3)/4;
        assert(r->bufferOffset==16 && !r->bufferRowLength && !r->bufferImageHeight);
        assert(r->imageSubresource.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT && r->imageSubresource.layerCount==1);
        assert(r->imageOffset.x==0 && r->imageOffset.y>=0 && r->imageOffset.y%4==0);
        assert(r->imageExtent.width==w && r->imageExtent.height>0 && r->imageExtent.depth==1);
        assert((unsigned)r->imageOffset.y+r->imageExtent.height<=h);
        assert(r->imageExtent.height%4==0 || (unsigned)r->imageOffset.y+r->imageExtent.height==h);
        assert((VkDeviceSize)bx*rows*8<=scratch_size);
        for(VkDeviceSize b=8;b<(VkDeviceSize)bx*rows*8;b+=8)assert(!memcmp(scratch+16,scratch+16+b,8));
        unsigned layer=image.target==NVN_TEXTURE_TARGET_3D ? (unsigned)r->imageOffset.z : r->imageSubresource.baseArrayLayer;
        assert(layer<8 && lv<3 && (unsigned)r->imageOffset.y/4+rows<8192);
        assert(image.target==NVN_TEXTURE_TARGET_3D ? r->imageSubresource.baseArrayLayer==0 : r->imageOffset.z==0);
        for(unsigned y=(unsigned)r->imageOffset.y/4;y<(unsigned)r->imageOffset.y/4+rows;y++) {
            assert(!covered[lv][layer][y]);covered[lv][layer][y]=1;
        }
        copies++;
    }
}
#define vkCmdCopyBufferToImage fake_copy_blocks
static VkImageSubresourceLayers sub_of(vkimg_t *im,const view_t *v,int has,int z) {
    return (VkImageSubresourceLayers){im->aspect,has && v->set_levels ? (uint32_t)v->base_level : 0,
        (has && v->set_layers ? (uint32_t)v->min_layer : 0)+(uint32_t)z,1};
}
static void write_region(unsigned lv,unsigned ly,VkRect2D r,unsigned mask,const VkClearColorValue *v) {
    for (unsigned y=0;y<r.extent.height;y++) for (unsigned x=0;x<r.extent.width;x++) for (unsigned c=0;c<4;c++)
        if (mask&(1u<<c)) pixels[lv][ly][r.offset.y+y][r.offset.x+x][c]=v->uint32[c];
}
static void fake_clear_image(VkCommandBuffer cb,VkImage im,VkImageLayout layout,const VkClearColorValue *v,uint32_t n,const VkImageSubresourceRange *r) {
    (void)cb;(void)im;(void)layout;transfers++;
    for (unsigned i=0;i<n;i++) for (unsigned lv=r[i].baseMipLevel;lv<r[i].baseMipLevel+r[i].levelCount;lv++)
        for(unsigned ly=r[i].baseArrayLayer;ly<r[i].baseArrayLayer+r[i].layerCount;ly++)
            write_region(lv,ly,(VkRect2D){{0,0},{image.w>>lv ? image.w>>lv : 1,image.h>>lv ? image.h>>lv : 1}},15,v);
}
static void fake_clear_depth(VkCommandBuffer cb,VkImage im,VkImageLayout layout,const VkClearDepthStencilValue *v,uint32_t n,const VkImageSubresourceRange *r) {
    (void)cb;(void)im;(void)layout;assert(n==1);depths++;depth_range=*r;depth_value=*v;
}
#define vkCmdClearColorImage fake_clear_image
#define vkCmdClearDepthStencilImage fake_clear_depth
static VkImageView vk_tex_view(const tex_t *t,const view_t *v,int dim,int arrayed,int attachment,vkimg_t **out) {
    (void)dim;(void)arrayed;(void)attachment;(void)out;view_level=v->base_level;view_layer=v->min_layer;
    if(fail_view)return VK_NULL_HANDLE;
    const vkfmt_t *f=vk_fmt(v->set_format ? v->format : t->d.format);if(!f)f=vk_fmt(t->d.format);
    vk_view_fmt=wrong_format ? image.fmt : f->vk;return (VkImageView)(uintptr_t)1;
}
static int clear_tex_slice(vkimg_t *im,VkImageView view,VkFormat format,VkExtent2D extent,VkRect2D rect,unsigned mask,unsigned type,const VkClearColorValue *v) {
    (void)im;(void)view;(void)format;(void)extent;(void)type;if(fail_draw)return 0;draws++;write_region(view_level,view_layer,rect,mask,v);return 1;
}
/* PRODUCTION_CLEAR */
static int compatibility(const char *name) {
    tex_t t={.d={.format=1}};
    image=(vkimg_t){.w=8,.h=8,.d=1,.layers=4,.levels=3,.samples=1,.target=4,.fmt=VK_FORMAT_R32G32B32A32_UINT,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,.nvn_format=1};
    p_clear_tex c={.tex=&t,.has_view=1,.view={.set_levels=1,.base_level=1,.nlevels=1,.set_layers=1,.min_layer=1,.nlayers=2},
        .r={1,1,0,2,2,2},.mask=1,.is_uint=1,.c.u={17,23,42,99}};
    int depth=strstr(name,"depth")!=NULL;
    int oned=!strcmp(name,"1d") || !strcmp(name,"1d_array");
    int full=!strcmp(name,"msaa_color") || !strcmp(name,"msaa_depth") || !strcmp(name,"msaa_view_format");
    if(depth){t.d.format=3;image.nvn_format=3;image.fmt=VK_FORMAT_D32_SFLOAT_S8_UINT;image.aspect=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;c.mask=0;c.c.f[0]=0.5f;c.c.u[1]=7;}
    if(full){image.samples=4;c.r.xoffset=c.r.yoffset=0;c.r.width=c.r.height=4;if(!depth)c.mask=15;}
    if(!strcmp(name,"msaa_view_format")){c.view.set_format=1;c.view.format=0;}
    if(!strcmp(name,"decoded")){t.d.format=4;image.nvn_format=4;image.fmt=VK_FORMAT_R32G32B32A32_SFLOAT;}
    if(!strcmp(name,"nonattachment"))image.usage=0;
    if(oned){image.target=!strcmp(name,"1d") ? NVN_TEXTURE_TARGET_1D : NVN_TEXTURE_TARGET_1D_ARRAY;image.h=1;c.r.yoffset=0;c.r.height=1;}
    if(!strcmp(name,"format")){c.view.set_format=1;c.view.format=0;wrong_format=1;}
    if(!strcmp(name,"view"))fail_view=1;
    if(!strcmp(name,"pipeline"))fail_draw=1;
    memset(pixels,0,sizeof pixels);clear_tex(&c);
    assert(image.gpu_w && draws==0);
    assert(full ? diagnostics==0 : diagnostics>0);
    if(depth){assert(depths==1 && transfers==0);assert(depth_value.depth==0.5f && depth_value.stencil==7);assert(depth_range.aspectMask==image.aspect && depth_range.baseMipLevel==1 && depth_range.levelCount==1 && depth_range.baseArrayLayer==1 && depth_range.layerCount==2);}
    else {assert(transfers==1);for(unsigned lv=0;lv<3;lv++)for(unsigned ly=0;ly<4;ly++)for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++)for(unsigned ch=0;ch<4;ch++){
        uint32_t expected=0;if(lv==1 && ly>=1 && ly<3 && x<4 && y<(oned ? 1u : 4u)){if(t.d.format==4){float value=(float)c.c.u[ch];memcpy(&expected,&value,4);}else expected=c.c.u[ch];}
        assert(pixels[lv][ly][y][x][ch]==expected);
    }}
    return 0;
}
static int layer_coordinates(const char *name) {
    tex_t t={.d={.format=1}};
    image=(vkimg_t){.w=8,.h=8,.d=1,.layers=1,.levels=3,.samples=1,.target=1,
        .fmt=VK_FORMAT_R32G32B32A32_UINT,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,
        .usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,.nvn_format=1};
    p_clear_tex c={.tex=&t,.has_view=1,.view={.set_levels=1,.base_level=1,.nlevels=1},
        .r={0,0,2,4,4,3},.mask=15,.is_uint=1,.c.u={17,23,42,99}};
    int partial=!strcmp(name,"single2d_partial"),depth=!strcmp(name,"single2d_depth");
    int invalid=strstr(name,"invalid_")!=NULL;
    if(partial){c.r.xoffset=c.r.yoffset=1;c.r.width=c.r.height=2;c.mask=1;}
    if(depth){t.d.format=image.nvn_format=3;image.fmt=VK_FORMAT_D32_SFLOAT_S8_UINT;
        image.aspect=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;c.mask=0;c.c.f[0]=0.5f;c.c.u[1]=7;}
    if(!strcmp(name,"invalid_array")){image.layers=4;image.target=4;c.r.zoffset=3;c.r.depth=2;}
    if(!strcmp(name,"invalid_volume")){image.target=NVN_TEXTURE_TARGET_3D;image.d=4;c.r.zoffset=2;c.r.depth=1;}
    if(!strcmp(name,"invalid_view")){image.layers=4;image.target=4;c.view.set_layers=1;c.view.min_layer=1;c.view.nlayers=1;c.r.zoffset=0;c.r.depth=2;}
    clear_tex(&c);
    if(invalid){assert(diagnostics && !draws && !transfers && !depths && !image.gpu_w);return 0;}
    assert(!diagnostics && image.gpu_w);
    if(depth){assert(depths==1 && !transfers && !draws);assert(depth_range.baseArrayLayer==0 && depth_range.layerCount==1 && depth_range.baseMipLevel==1);return 0;}
    assert(partial ? draws==1 && !transfers : transfers==1 && !draws);
    for(unsigned lv=0;lv<3;lv++)for(unsigned ly=0;ly<4;ly++)for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++)for(unsigned ch=0;ch<4;ch++){
        int inside=partial ? x>=1 && x<3 && y>=1 && y<3 && ch==0 : x<4 && y<4;
        uint32_t expected=lv==1 && ly==0 && inside ? c.c.u[ch] : 0;
        assert(pixels[lv][ly][y][x][ch]==expected);
    }
    return 0;
}
static int compressed_case(const char *name) {
    tex_t t={.d={.format=5}};
    image=(vkimg_t){.w=13,.h=9,.d=1,.layers=4,.levels=3,.samples=1,.target=4,.fmt=VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        .aspect=VK_IMAGE_ASPECT_COLOR_BIT,.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT,.nvn_format=5};
    p_clear_tex c={.tex=&t,.has_view=1,.view={.set_levels=1,.base_level=1,.nlevels=1,.set_layers=1,.min_layer=1,.nlayers=2},
        .r={1,1,0,2,2,2},.mask=1,.is_uint=1,.c.u={1,1,1,1}};
    unsigned level=1,first=1,count=2;
    int full=!strcmp(name,"compressed_full"),noop=!strcmp(name,"compressed_mask0");
    int invalid=!strcmp(name,"compressed_msaa") || !strcmp(name,"compressed_bad_usage");
    if(full){c.r=(NVNcopyRegion){0,0,0,6,4,2};c.mask=15;}
    if(!strcmp(name,"compressed_single2d")){image.layers=1;image.target=1;c.view.set_layers=0;c.r.zoffset=2;c.r.depth=3;first=0;count=1;}
    if(!strcmp(name,"compressed_volume")){image.target=NVN_TEXTURE_TARGET_3D;image.d=8;image.layers=1;c.r.zoffset=1;c.r.depth=1;first=0;count=4;}
    if(!strcmp(name,"compressed_tail")){level=2;c.view.base_level=2;}
    if(!strcmp(name,"compressed_chunks")){image.w=4096;image.h=129;level=0;c.view.base_level=0;}
    if(!strcmp(name,"compressed_unknown_format")){c.view.set_format=1;c.view.format=999;}
    if(noop)c.mask=0;
    if(!strcmp(name,"compressed_msaa"))image.samples=4;
    if(!strcmp(name,"compressed_bad_usage"))image.usage=0;
    clear_tex(&c);
    assert(!transfers && !depths && !draws);
    if(noop || invalid){assert(!copies && !scratch_calls && !image.gpu_w);return 0;}
    assert(copies && scratch_calls==1 && image.gpu_w);
    assert(full ? !diagnostics : diagnostics>0);
    unsigned h=image.h>>level ? image.h>>level : 1,rows=(h+3)/4;
    for(unsigned lv=0;lv<3;lv++)for(unsigned ly=0;ly<8;ly++)for(unsigned y=0;y<8192;y++)
        assert(covered[lv][ly][y]==(lv==level && ly>=first && ly<first+count && y<rows));
    return 0;
}
int main(int argc,char **argv) {
    if(argc>1){
        if(strstr(argv[1],"compressed"))return compressed_case(argv[1]);
        if(strstr(argv[1],"single2d") || strstr(argv[1],"invalid_"))return layer_coordinates(argv[1]);
        return compatibility(argv[1]);
    }
    tex_t t={.d={.format=1}};
    image=(vkimg_t){.w=8,.h=8,.d=1,.layers=4,.levels=3,.samples=1,.target=4,.fmt=VK_FORMAT_R32G32B32A32_UINT,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,.nvn_format=1};
    for (int unknown=0;unknown<2;unknown++) for (int kind=0;kind<3;kind++) for (unsigned mask=0;mask<16;mask++) {
        for (unsigned lv=0;lv<3;lv++) for(unsigned ly=0;ly<4;ly++) for(unsigned y=0;y<8;y++) for(unsigned x=0;x<8;x++)
            for(unsigned c=0;c<4;c++) pixels[lv][ly][y][x][c]=0x12345678u+c;
        t.d.format=kind;image.fmt=vk_fmt(kind)->vk;image.nvn_format=kind;
        p_clear_tex clear={.tex=&t,.has_view=1,.view={.set_levels=1,.base_level=1,.nlevels=1,.set_layers=1,.min_layer=1,.nlayers=2},
            .r={1,1,0,2,2,1},.mask=(int)mask,.is_uint=1,.c.u={17,23,42,99}};
        clear.view.set_format=unknown;clear.view.format=999;
        clear_tex(&clear);
        VkClearColorValue want;
        if(kind)memcpy(want.uint32,clear.c.u,16);else for(int c=0;c<4;c++)want.float32[c]=(float)clear.c.u[c];
        for(unsigned lv=0;lv<3;lv++)for(unsigned ly=0;ly<4;ly++)for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++)for(unsigned c=0;c<4;c++) {
            uint32_t expected=lv==1 && ly==1 && x>=1 && x<3 && y>=1 && y<3 && (mask&(1u<<c)) ? want.uint32[c] : 0x12345678u+c;
            if(pixels[lv][ly][y][x][c]!=expected) {fprintf(stderr,"clear kind%d mask%u level%u layer%u pixel%u,%u channel%u: %#x expected %#x\n",kind,mask,lv,ly,x,y,c,pixels[lv][ly][y][x][c],expected);return 1;}
        }
    }
    assert(!diagnostics);
    p_clear_tex full={.tex=&t,.r={0,0,0,8,8,1},.mask=15,.is_uint=1,.c.u={3,5,7,9}};
    full.has_view=1;full.view.set_format=1;full.view.format=999;
    transfers=draws=0;clear_tex(&full);assert(transfers==1 && draws==0);
    image.samples=4;transfers=draws=0;clear_tex(&full);assert(transfers==1 && draws==0);image.samples=1;
    // Invalid/empty ranges must never reach either Vulkan write path.
    for(int bad=0;bad<8;bad++) {
        p_clear_tex c=full;
        if(bad==0)c.mask=0;
        if(bad==1)c.r.width=0;
        if(bad==2)c.r.xoffset=INT32_MAX;
        if(bad==3)c.r.zoffset=-1;
        if(bad==4)c.r.depth=-1;
        if(bad==5)c.r.zoffset=4;
        if(bad==6){c.has_view=1;c.view.set_levels=1;c.view.base_level=3;}
        if(bad==7){c.has_view=1;c.view.set_layers=1;c.view.min_layer=3;c.view.nlayers=1;c.r.depth=2;}
        transfers=draws=0;clear_tex(&c);assert(transfers==0 && draws==0);
        image.target=4;
    }
    // Float-entry integer clears convert numerically, including signed values.
    p_clear_tex numeric=full;numeric.is_uint=0;numeric.c.f[0]=-17.75f;
    numeric.c.f[1]=23.5f;numeric.c.f[2]=42.0f;numeric.c.f[3]=99.0f;
    clear_tex(&numeric);assert(pixels[0][0][0][0][0]==(uint32_t)-17 && pixels[0][0][0][0][1]==23);
    t.d.format=1;image.nvn_format=1;image.fmt=vk_fmt(1)->vk;clear_tex(&numeric);assert(pixels[0][0][0][0][0]==0);
    puts("ClearTexture: format/mask guards, empty/invalid ranges, typed conversion and transfer/draw routing passed");
    return 0;
}
