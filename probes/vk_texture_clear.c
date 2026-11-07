// Used by vk_texture_clear.py. The marked sections come from production source.
/* TYPES */
#include "nvn_hmap.h"
#include "maskclear_vert.spv.h"
#include "maskclear_frag.spv.h"
#include "maskclear_uint_frag.spv.h"
#include "maskclear_sint_frag.spv.h"
#define CHECK(call) do { VkResult result=(call); if(result) {fprintf(stderr,"%s: %d\n",#call,result);exit(1);} } while(0)
#define VKRED_ONCE(id, ...) do { fprintf(stderr,"[RED] " __VA_ARGS__);fputc('\n',stderr);exit(1); } while(0)
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
    {VK_FORMAT_R32G32B32A32_SFLOAT,0,0,0,0}, {VK_FORMAT_R32G32B32A32_UINT,1,0,0,0},
    {VK_FORMAT_R32G32B32A32_SINT,1,1,0,0}, {VK_FORMAT_R8G8B8A8_UNORM,0,0,0,0},
    {VK_FORMAT_R8G8B8A8_SRGB,0,0,0,0}};
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
static void run_case(int kind,unsigned mask,int shape,int scenario) {
    int volume=shape==2;unsigned layers=shape==1 ? 6 : 4, pixel_bytes=kind<3 ? 16 : 4;
    image=(vkimg_t){.w=8,.h=8,.d=volume?8:1,.layers=volume?1:layers,.levels=3,.samples=1,
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
int main(int argc,char **argv) {
    legacy=argc>1 && !strcmp(argv[1],"legacy");
    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO,NULL,"texture-clear",1,NULL,0,VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,NULL,0,&app};VkInstance instance;CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=1;CHECK(vkEnumeratePhysicalDevices(instance,&count,&V.pd));if(!count)return 1;
    VkPhysicalDeviceProperties prop;vkGetPhysicalDeviceProperties(V.pd,&prop);printf("%s: %s\n",legacy?"legacy":"dynamic",prop.deviceName);
    VkPhysicalDeviceDynamicRenderingFeatures dynamic={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceFeatures2 features={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&dynamic};vkGetPhysicalDeviceFeatures2(V.pd,&features);
    V.has_dynrender=dynamic.dynamicRendering;if(!legacy && !V.has_dynrender){fprintf(stderr,"Dynamic rendering unavailable\n");return 1;}
    uint32_t nq=0;vkGetPhysicalDeviceQueueFamilyProperties(V.pd,&nq,NULL);VkQueueFamilyProperties *qs=calloc(nq,sizeof *qs);vkGetPhysicalDeviceQueueFamilyProperties(V.pd,&nq,qs);
    uint32_t qf=0;while(qf<nq && !(qs[qf].queueFlags&VK_QUEUE_GRAPHICS_BIT))qf++;free(qs);if(qf==nq)return 1;
    float priority=1;VkDeviceQueueCreateInfo qi={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,NULL,0,qf,1,&priority};
    VkDeviceCreateInfo di={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&dynamic,0,1,&qi};CHECK(vkCreateDevice(V.pd,&di,NULL,&V.dev));vkGetDeviceQueue(V.dev,qf,0,&queue);
    begin_rendering=(PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(V.dev,"vkCmdBeginRendering");end_rendering=(PFN_vkCmdEndRendering)vkGetDeviceProcAddr(V.dev,"vkCmdEndRendering");
    VkCommandPoolCreateInfo pi={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,NULL,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,qf};VkCommandPool pool;CHECK(vkCreateCommandPool(V.dev,&pi,NULL,&pool));
    VkCommandBufferAllocateInfo ai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,NULL,pool,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};CHECK(vkAllocateCommandBuffers(V.dev,&ai,&command));
    unsigned tests=0;for(int kind=0;kind<5;kind++)for(int shape=0;shape<3;shape++)for(int scenario=0;scenario<4;scenario++)for(unsigned mask=0;mask<16;mask++){run_case(kind,mask,shape,scenario);tests++;}
    printf("PASS: %u clear/readback cases; untouched channels, pixels, layers and mips preserved\n",tests);
    for(unsigned i=0;i<g_mc_pipes.cap;i++)if(g_mc_pipes.vals[i])vkDestroyPipeline(V.dev,g_mc_pipes.vals[i],NULL);
    free(g_mc_pipes.keys);free(g_mc_pipes.vals);
    for(unsigned i=0;i<3;i++)vkDestroyShaderModule(V.dev,g_mc.fs[i],NULL);vkDestroyShaderModule(V.dev,g_mc.vs,NULL);vkDestroyPipelineLayout(V.dev,g_mc.pl,NULL);
    for(unsigned i=0;i<5;i++)if(passes[i])vkDestroyRenderPass(V.dev,passes[i],NULL);
    vkDestroyCommandPool(V.dev,pool,NULL);vkDestroyDevice(V.dev,NULL);vkDestroyInstance(instance,NULL);return 0;
}
