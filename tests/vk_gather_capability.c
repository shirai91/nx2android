// Copied nxnative texture_ops.c at 1f01e28c87da5233dfa14fb31fabf1535df33013.
// Only DMSV loading and feature-disable control differ from the donor runner.
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include "texture_ops_matrix.h"
#define CHECK(call) do { VkResult r_=(call); if(r_) { fprintf(stderr,"%s: %d\n",#call,r_);return 1; } } while(0)
typedef struct { unsigned uniform,storage,image,support,driver; int has_support,has_driver,extended_gather; } bindings_t;
// Adapter: read the uncompressed target DMSV bundle, not donor .vk.txt files.
static uint32_t u32(const void *p) { uint32_t v;memcpy(&v,p,4);return v; }
static uint16_t u16(const void *p) { uint16_t v;memcpy(&v,p,2);return v; }
static int load(VkDevice device,const char *path,unsigned variant,VkShaderModule *module,bindings_t *bindings,int extended_gather) {
    FILE *file=fopen(path,"rb");if(!file)return 1;
    if(fseek(file,0,SEEK_END)) { fclose(file);return 1; }
    long bytes=ftell(file);if(bytes<12 || fseek(file,0,SEEK_SET)) { fclose(file);return 1; }
    unsigned char *data=malloc((size_t)bytes);if(!data) { fclose(file);return 1; }
    int ok=fread(data,1,(size_t)bytes,file)==(size_t)bytes;fclose(file);
    if(!ok || memcmp(data,"DMSV",4) || u32(data+4)!=1) { free(data);return 1; }
    uint32_t count=u32(data+8);if(count>((size_t)bytes-12)/32) { free(data);return 1; }
    const unsigned char *entry=NULL;
    for(uint32_t i=0;i<count;i++) {
        const unsigned char *e=data+12+32*i;
        if(u32(e)==0xfeed7000u+variant && !u32(e+4))entry=e;
    }
    if(!entry || u32(entry+8)!=5 || u32(entry+28)) { free(data);return 1; }
    uint32_t offset=u32(entry+12),size=u32(entry+16),ro=u32(entry+20),rs=u32(entry+24);
    if(offset%4 || size<20 || size%4 || (uint64_t)offset+size>(size_t)bytes || rs<20 || (uint64_t)ro+rs>(size_t)bytes) { free(data);return 1; }
    const uint32_t *code=(const uint32_t*)(data+offset);const unsigned char *reflection=data+ro;
    uint32_t nbind=u32(reflection);if(nbind>(rs-20)/20 || u32(reflection+4)) { free(data);return 1; }
    for(unsigned i=0;i<3;i++)if(u32(reflection+8+4*i)!=1) { free(data);return 1; }
    unsigned seen=0;
    for(uint32_t i=0;i<nbind;i++) {
        const unsigned char *b=reflection+20+20*i;
        unsigned binding=u16(b+4),n=u16(b+6);int reg=(int16_t)u16(b+8),off=(int32_t)u32(b+12);
        if(b[0]==1 && b[1]==0 && n==1 && reg==0) { bindings->uniform=binding;seen|=1; }
        else if(b[0]==6 && b[1]==1 && n==1 && reg==0 && off==56) { bindings->storage=binding;seen|=2; }
        else if(b[0]==7 && b[1]==2 && n==1 && reg==128 && b[10]==2 && u32(b+16)==1) { bindings->image=binding;seen|=4; }
        else if(b[0]==3 && b[1]==0 && n==1 && reg==2) { bindings->driver=binding;bindings->has_driver=1; }
        else if(b[0]==0 && b[1]==0 && n==1) { bindings->support=binding;bindings->has_support=1; }
        else { free(data);return 1; }
    }
    if(seen!=7 || code[0]!=0x07230203) { free(data);return 1; }
    for(uint32_t at=5;at<size/4;) {
        unsigned n=code[at]>>16,op=code[at]&0xffff;
        if(!n || n>size/4-at) { free(data);return 1; }
        if(op==17 && n==2 && code[at+1]==25)bindings->extended_gather=1;
        at+=n;
    }
    if(bindings->extended_gather && !extended_gather) { fprintf(stderr,"Extended texture gather unsupported\n");free(data);return 77; }
    VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size,.pCode=code};
    VkResult result=vkCreateShaderModule(device,&ci,NULL,module);free(data);CHECK(result);return 0;
}
static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties *memory,uint32_t bits,VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<memory->memoryTypeCount;i++) if((bits&(1u<<i)) && (memory->memoryTypes[i].propertyFlags&flags)==flags)return i;
    fprintf(stderr,"No compatible memory type\n");exit(1);
}
int main(int argc,char **argv) {
    if(argc<3 || argc>4)return 2;unsigned variant=(unsigned)strtoul(argv[2],NULL,10),mode=argc==4?(unsigned)strtoul(argv[3],NULL,10):0;
    if(variant>=TEX_OP_VARIANTS || mode>7 || (mode && variant!=1 && variant!=3 && variant!=9))return 2;
    tex_config_t config=tex_config(mode);
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};VkInstance instance;CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=1;VkPhysicalDevice physical;VkResult picked=vkEnumeratePhysicalDevices(instance,&count,&physical);
    if((picked!=VK_SUCCESS && picked!=VK_INCOMPLETE)||!count)return 1;
    VkPhysicalDeviceFeatures features;vkGetPhysicalDeviceFeatures(physical,&features);
    features.shaderImageGatherExtended=VK_FALSE; // Deliberately disable the feature under test.
    VkFormatProperties format;vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R32G32B32A32_SFLOAT,&format);
    if(!(format.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) || ((config.min_linear || config.mag_linear) && !(format.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)))return 77;
    VkQueueFamilyProperties families[32];count=32;vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families);
    uint32_t family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;if(family==count)return 77;
    float priority=1;VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pEnabledFeatures=&features,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    VkDevice device;CHECK(vkCreateDevice(physical,&dci,NULL,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    bindings_t bindings={0};VkShaderModule shader;int loaded=load(device,argv[1],variant,&shader,&bindings,features.shaderImageGatherExtended);if(loaded)return loaded;
    VkBuffer buffers[4];VkDeviceMemory allocations[4];uint32_t *mapped[4];const unsigned sizes[]={65536,1024,4096,4096};
    for(unsigned i=0;i<4;i++) {
        VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=sizes[i],.usage=i==1?VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:i==3?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        CHECK(vkCreateBuffer(device,&ci,NULL,&buffers[i]));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,buffers[i],&req);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(&memory,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CHECK(vkAllocateMemory(device,&ai,NULL,&allocations[i]));CHECK(vkBindBufferMemory(device,buffers[i],allocations[i],0));CHECK(vkMapMemory(device,allocations[i],0,VK_WHOLE_SIZE,0,(void**)&mapped[i]));
    }
    memset(mapped[2],0,sizes[2]);for(unsigned i=0;i<73*4;i++)mapped[2][48+i]=tex_bits(1);
    VkImage image;VkDeviceMemory image_memory;VkImageView view;VkSampler samplers[2];
    VkImageCreateInfo imci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R32G32B32A32_SFLOAT,.extent={config.width,config.height,1},.mipLevels=config.levels,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateImage(device,&imci,NULL,&image));VkMemoryRequirements req;vkGetImageMemoryRequirements(device,image,&req);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=memory_type(&memory,req.memoryTypeBits,0)};
    CHECK(vkAllocateMemory(device,&ai,NULL,&image_memory));CHECK(vkBindImageMemory(device,image,image_memory,0));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=VK_FORMAT_R32G32B32A32_SFLOAT,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,config.base,config.levels-config.base,0,1}};
    CHECK(vkCreateImageView(device,&vi,NULL,&view));
    for(unsigned i=0;i<2;i++) {
        VkSamplerCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.minFilter=config.min_linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST,.magFilter=config.mag_linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST,.mipmapMode=config.mip_linear?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU=i?VK_SAMPLER_ADDRESS_MODE_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=i?VK_SAMPLER_ADDRESS_MODE_REPEAT:VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.minLod=config.min_lod,.maxLod=config.max_lod};
        CHECK(vkCreateSampler(device,&ci,NULL,&samplers[i]));
    }
    if(!bindings.has_support || !bindings.has_driver)return 1;
    unsigned binding_count=5; // Target retains the driver c2 binding, unlike donor optimization.
    VkDescriptorSetLayoutBinding lb[]={{bindings.uniform,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {bindings.storage,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},{bindings.image,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {bindings.support,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
        {bindings.driver,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo slci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=binding_count,.pBindings=lb};
    VkDescriptorSetLayout set_layout;CHECK(vkCreateDescriptorSetLayout(device,&slci,NULL,&set_layout));
    VkDescriptorPoolSize ps[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,3},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1}};
    VkDescriptorPoolCreateInfo dpci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=3,.pPoolSizes=ps};
    VkDescriptorPool pool;CHECK(vkCreateDescriptorPool(device,&dpci,NULL,&pool));
    VkDescriptorSetAllocateInfo dsai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=pool,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set;CHECK(vkAllocateDescriptorSets(device,&dsai,&set));
    VkDescriptorBufferInfo bi[]={{buffers[0],0,sizes[0]},{buffers[1],256,16},{buffers[2],0,sizes[2]}};
    VkDescriptorImageInfo image_info={samplers[0],view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[5];
    for(unsigned i=0;i<binding_count;i++)writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=lb[i].binding,.descriptorCount=1,.descriptorType=lb[i].descriptorType,.pBufferInfo=i==2?NULL:&bi[i==3?2:i==4?0:i],.pImageInfo=i==2?&image_info:NULL};
    vkUpdateDescriptorSets(device,binding_count,writes,0,NULL);
    VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout};VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(device,&plci,NULL,&layout));
    VkComputePipelineCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=layout,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"}};
    VkPipeline pipeline;CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,NULL,&pipeline));
    VkCommandPoolCreateInfo cpci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=family};VkCommandPool commands;CHECK(vkCreateCommandPool(device,&cpci,NULL,&commands));
    VkCommandBufferAllocateInfo cbai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=commands,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};VkCommandBuffer command;CHECK(vkAllocateCommandBuffers(device,&cbai,&command));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(command,&begin));
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,config.levels,0,1}};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    VkBufferImageCopy copies[5];unsigned offset=0;
    for(unsigned mip=0;mip<config.levels;mip++) {
        unsigned width=tex_dimension(config.width,mip),height=tex_dimension(config.height,mip);tex_matrix_level((float*)((char*)mapped[3]+offset),mode,mip,config);
        copies[mip]=(VkBufferImageCopy){.bufferOffset=offset,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,1},.imageExtent={width,height,1}};
        offset+=width*height*16;
    }
    vkCmdCopyBufferToImage(command,buffers[3],image,VK_IMAGE_LAYOUT_GENERAL,config.levels,copies);
    VkMemoryBarrier ready={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
    for(unsigned index=0;index<tex_matrix_count(mode);index++) {
        uint32_t expected[4];tex_matrix_inputs(variant,mode,index,config,mapped[0],expected);memset(mapped[1],0xcc,sizes[1]);
        image_info.sampler=samplers[tex_matrix_wrap(mode,index)];vkUpdateDescriptorSets(device,1,&writes[2],0,NULL);
        CHECK(vkResetCommandBuffer(command,0));CHECK(vkBeginCommandBuffer(command,&begin));vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,NULL);vkCmdDispatch(command,1,1,1);
        VkMemoryBarrier done={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&done,0,NULL,0,NULL);CHECK(vkEndCommandBuffer(command));CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
        if(tex_matrix_check(variant,mode,index,mapped[1],expected))return 1;
    }
    vkDestroyCommandPool(device,commands,NULL);vkDestroyPipeline(device,pipeline,NULL);vkDestroyPipelineLayout(device,layout,NULL);vkDestroyShaderModule(device,shader,NULL);vkDestroyDescriptorPool(device,pool,NULL);vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    for(unsigned i=0;i<2;i++)vkDestroySampler(device,samplers[i],NULL);
    vkDestroyImageView(device,view,NULL);vkDestroyImage(device,image,NULL);vkFreeMemory(device,image_memory,NULL);
    for(unsigned i=0;i<4;i++) { vkUnmapMemory(device,allocations[i]);vkDestroyBuffer(device,buffers[i],NULL);vkFreeMemory(device,allocations[i],NULL); }
    vkDestroyDevice(device,NULL);vkDestroyInstance(instance,NULL);fprintf(stderr,"Vulkan texture variant%u mode%u: %u cases passed\n",variant,mode,tex_matrix_count(mode));return 0;
}
