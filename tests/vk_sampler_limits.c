// Donor bbd6424db sampler_limits.inc cases, adapted to the actual sampler
// creation seam. The driver still creates every sampler in REAL_VULKAN mode.
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>
static struct {VkDevice dev; VkPhysicalDeviceProperties props; float max_aniso; int has_custom_border;} V;
static VkPhysicalDeviceFeatures2 ff={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
static VkSamplerCreateInfo captured;
static int calls;
static VkResult capture_sampler(VkDevice dev,const VkSamplerCreateInfo *info,const VkAllocationCallbacks *allocator,VkSampler *out) {
    captured=*info;calls++;
#ifdef REAL_VULKAN
    return vkCreateSampler(dev,info,allocator,out);
#else
    (void)dev;(void)allocator;*out=(VkSampler)(uintptr_t)calls;return VK_SUCCESS;
#endif
}
#define vkCreateSampler capture_sampler
#include "sampler_production.h"
#undef vkCreateSampler
static void check(const char *what,float got,float want) {
    if(fabsf(got-want)>0.00001f || !isfinite(got)) {fprintf(stderr,"%s: got %g expected %g\n",what,got,want);exit(1);}
}
int main(int argc,char **argv) {
    assert(argc==3);int mode=atoi(argv[1]),row=atoi(argv[2]);
#ifdef REAL_VULKAN
    VkApplicationInfo application={VK_STRUCTURE_TYPE_APPLICATION_INFO,NULL,"sampler-limit-probe",0,NULL,0,VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance_info={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,NULL,0,&application};
    VkInstance instance;assert(vkCreateInstance(&instance_info,NULL,&instance)==VK_SUCCESS);
    uint32_t count=1;VkPhysicalDevice device;assert(vkEnumeratePhysicalDevices(instance,&count,&device)==VK_SUCCESS && count);
    vkGetPhysicalDeviceProperties(device,&V.props);vkGetPhysicalDeviceFeatures2(device,&ff);
    uint32_t family_count=0;vkGetPhysicalDeviceQueueFamilyProperties(device,&family_count,NULL);
    VkQueueFamilyProperties *families=calloc(family_count,sizeof *families);assert(families);
    vkGetPhysicalDeviceQueueFamilyProperties(device,&family_count,families);unsigned family=0;
    while(family<family_count && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))family++;
    assert(family<family_count);free(families);
#else
    V.props.limits.maxSamplerAnisotropy=64;V.props.limits.maxSamplerLodBias=32;ff.features.samplerAnisotropy=VK_TRUE;
#endif
    if(mode==1) {V.props.limits.maxSamplerAnisotropy=fminf(2,V.props.limits.maxSamplerAnisotropy);V.props.limits.maxSamplerLodBias=fminf(2,V.props.limits.maxSamplerLodBias);}
    if(mode==2) ff.features.samplerAnisotropy=VK_FALSE;
    init_limits();
#ifdef REAL_VULKAN
    float priority=1;VkDeviceQueueCreateInfo queue={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,NULL,0,family,1,&priority};
    VkDeviceCreateInfo device_info={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&ff,0,1,&queue};
    assert(vkCreateDevice(device,&device_info,NULL,&V.dev)==VK_SUCCESS);
#endif
    // Input bias/aniso from the donor; remaining rows exercise its LOD rules.
    static const struct {float bias,aniso,lo,hi,expected_bias,expected_aniso,expected_lo,expected_hi;int minf;} cases[]={
        {90,64,-5,2,15.99f,16,0,2,5}, {-90,8,-5,2,-16,8,0,2,5},
        {1.25f,3.75f,-5,2,1.25f,3,0,2,5}, {-.75f,1.75f,-5,2,-.75f,1,0,2,5},
        {0,0,7,2,0,1,0,.25f,0}, {0,-3,7,2,0,1,0,.25f,1},
        {0,1,7,2,0,1,7,7,2}, {0,1,-7,-2,0,1,0,0,3}, {0,1,2,7,0,1,2,7,4}
    };
    assert(row>=0 && row<(int)(sizeof cases/sizeof *cases));
    const typeof(*cases) *c=&cases[row];
    samp_par p={.ws=6,.wt=1,.wr=0,.magf=1,.minf=c->minf,.lod_bias=c->bias,.lod_min=c->lo,.lod_max=c->hi,.aniso=c->aniso};
    VkSampler sampler=make_sampler(&p);assert(sampler && calls==1);
    float bias=c->expected_bias,aniso=c->expected_aniso;
    if(bias>V.props.limits.maxSamplerLodBias)bias=V.props.limits.maxSamplerLodBias;
    if(bias<-V.props.limits.maxSamplerLodBias)bias=-V.props.limits.maxSamplerLodBias;
    if(aniso>V.props.limits.maxSamplerAnisotropy)aniso=V.props.limits.maxSamplerAnisotropy;
    if(!ff.features.samplerAnisotropy)aniso=1;
    check("bias",captured.mipLodBias,bias);check("minimum LOD",captured.minLod,c->expected_lo);check("maximum LOD",captured.maxLod,c->expected_hi);
    check("anisotropy",captured.maxAnisotropy,aniso);assert(captured.anisotropyEnable==(aniso>1));
    assert(captured.addressModeU==VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT && captured.addressModeV==VK_SAMPLER_ADDRESS_MODE_REPEAT);
#ifdef REAL_VULKAN
    vkDestroySampler(V.dev,sampler,NULL);vkDestroyDevice(V.dev,NULL);vkDestroyInstance(instance,NULL);
#endif
    printf("sampler limits mode=%d case=%d passed\n",mode,row);
}
