// Standalone Vulkan compiler inspection. Never creates command buffers or submits GPU work.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult r_=(x); if(r_!=VK_SUCCESS){fprintf(stderr,"[RED] %s -> %d\n",#x,r_);exit(2);} } while(0)
#define NEED(x) do { if(!(x)){fprintf(stderr,"[RED] %s: %s\n",#x,dlerror());exit(3);} } while(0)
static PFN_vkGetInstanceProcAddr gip;
static PFN_vkGetDeviceProcAddr gdp;
static VkInstance inst;
static VkDevice dev;
#define LOAD_I(n) PFN_##n n=(PFN_##n)gip(inst,#n);NEED(n)
#define LOAD_D(n) PFN_##n n=(PFN_##n)gdp(dev,#n);NEED(n)

static VkShaderModule module(const char *root,const char *name) {
    LOAD_D(vkCreateShaderModule);
    char path[1200];snprintf(path,sizeof path,"%s/%s",root,name);FILE *f=fopen(path,"rb");assert(f);
    fseek(f,0,SEEK_END);long size=ftell(f);assert(size>=20 && size<=1<<20 && !(size&3));rewind(f);
    uint32_t *w=malloc(size);assert(w && fread(w,1,size,f)==(size_t)size);fclose(f);assert(w[0]==0x07230203);
    VkShaderModuleCreateInfo ci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,(size_t)size,w};
    VkShaderModule m;CHECK(vkCreateShaderModule(dev,&ci,NULL,&m));free(w);return m;
}
static void dump(const char *root,const char *label,VkPipeline pipe) {
    LOAD_D(vkGetPipelineExecutablePropertiesKHR);LOAD_D(vkGetPipelineExecutableStatisticsKHR);LOAD_D(vkGetPipelineExecutableInternalRepresentationsKHR);
    char path[1200];snprintf(path,sizeof path,"%s/%s.txt",root,label);FILE *f=fopen(path,"w");assert(f);
    VkPipelineInfoKHR pi={VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR,NULL,pipe};uint32_t n=0;
    CHECK(vkGetPipelineExecutablePropertiesKHR(dev,&pi,&n,NULL));assert(n>0 && n<=16);
    VkPipelineExecutablePropertiesKHR props[16]={0};for(uint32_t i=0;i<n;i++)props[i].sType=VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    CHECK(vkGetPipelineExecutablePropertiesKHR(dev,&pi,&n,props));
    for(uint32_t i=0;i<n;i++) {
        fprintf(f,"EXEC %u stages %#x subgroup %u %s\n",i,props[i].stages,props[i].subgroupSize,props[i].name);
        VkPipelineExecutableInfoKHR ei={VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,NULL,pipe,i};uint32_t ns=0;
        CHECK(vkGetPipelineExecutableStatisticsKHR(dev,&ei,&ns,NULL));assert(ns<=256);
        VkPipelineExecutableStatisticKHR stats[256]={0};for(uint32_t j=0;j<ns;j++)stats[j].sType=VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
        CHECK(vkGetPipelineExecutableStatisticsKHR(dev,&ei,&ns,stats));
        for(uint32_t j=0;j<ns;j++) {
            fprintf(f,"STAT %s = ",stats[j].name);
            switch(stats[j].format) {
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:fprintf(f,"%u",stats[j].value.b32);break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:fprintf(f,"%"PRIu64,stats[j].value.u64);break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:fprintf(f,"%"PRId64,stats[j].value.i64);break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:fprintf(f,"%.17g",stats[j].value.f64);break;
                default:assert(0);
            }fputc('\n',f);
        }
        uint32_t nr=0;CHECK(vkGetPipelineExecutableInternalRepresentationsKHR(dev,&ei,&nr,NULL));assert(nr<=16);
        fprintf(f,"REPRESENTATIONS %u\n",nr);
        VkPipelineExecutableInternalRepresentationKHR repr[16]={0};for(uint32_t j=0;j<nr;j++)repr[j].sType=VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;
        CHECK(vkGetPipelineExecutableInternalRepresentationsKHR(dev,&ei,&nr,repr));
        size_t sizes[16];for(uint32_t j=0;j<nr;j++){sizes[j]=repr[j].dataSize;assert(sizes[j]>0 && sizes[j]<=8u<<20);repr[j].pData=malloc(sizes[j]);assert(repr[j].pData);}
        uint32_t filled=nr;CHECK(vkGetPipelineExecutableInternalRepresentationsKHR(dev,&ei,&filled,repr));assert(filled<=nr);
        for(uint32_t j=0;j<filled;j++) {
            assert(repr[j].dataSize<=sizes[j]);snprintf(path,sizeof path,"%s/%s-%u-%u.ir",root,label,i,j);
            FILE *ir=fopen(path,"wb");assert(ir && fwrite(repr[j].pData,1,repr[j].dataSize,ir)==repr[j].dataSize);assert(!fclose(ir));
            fprintf(f,"REP %u text %u bytes %zu name %s description %s\n",j,repr[j].isText,repr[j].dataSize,repr[j].name,repr[j].description);
        }for(uint32_t j=0;j<nr;j++)free(repr[j].pData);
    }assert(!fclose(f));
}
static void compile_cases(const char *root,const char *spec) {
    LOAD_D(vkCreateDescriptorSetLayout);LOAD_D(vkCreatePipelineLayout);LOAD_D(vkCreateGraphicsPipelines);
    LOAD_D(vkDestroyPipeline);LOAD_D(vkDestroyPipelineLayout);LOAD_D(vkDestroyDescriptorSetLayout);LOAD_D(vkDestroyShaderModule);
    FILE *f=fopen(spec,"r");assert(f);unsigned count;assert(fscanf(f,"%u",&count)==1 && count<=16);
    for(unsigned c=0;c<count;c++) {
        char label[128],vp[128],fp[128];unsigned nv,nf,na,nb,nc,dfmt,dt,dw,df,topo,samples;
        assert(fscanf(f,"%127s %127s %127s %u %u %u %u %u %u %u %u %u %u %u",label,vp,fp,&nv,&nf,&na,&nb,&nc,&dfmt,&dt,&dw,&df,&topo,&samples)==14);assert(nv<=32 && nf<=32 && na<=16 && nb<=17 && nc<=8 && dt<=1 && dw<=1 && df<=7 && topo<=10 && (samples==1 || samples==2 || samples==4 || samples==8)); assert(!strchr(label,'/') && !strchr(label,'\\') && !strstr(label,".."));
        VkDescriptorSetLayout dsl[2];
        for(unsigned st=0;st<2;st++) {
            unsigned num=st?nf:nv;VkDescriptorSetLayoutBinding b[32]={0};
            for(unsigned j=0;j<num;j++){
                unsigned kind,n;assert(fscanf(f,"%u %u %u",&b[j].binding,&kind,&n)==3);assert(kind<=10 && n<=64 && n>0);
                b[j].descriptorType=(VkDescriptorType)kind;b[j].descriptorCount=n;b[j].stageFlags=st?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;
            }VkDescriptorSetLayoutCreateInfo ci={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,NULL,0,num,b};CHECK(vkCreateDescriptorSetLayout(dev,&ci,NULL,&dsl[st]));
        }
        VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkVertexInputAttributeDescription attrs[16];VkVertexInputBindingDescription bindings[17];
        for(unsigned j=0;j<na;j++){unsigned fmt;assert(fscanf(f,"%u %u %u %u",&attrs[j].location,&attrs[j].binding,&fmt,&attrs[j].offset)==4);attrs[j].format=(VkFormat)fmt;}
        for(unsigned j=0;j<nb;j++){unsigned rate;assert(fscanf(f,"%u %u %u",&bindings[j].binding,&bindings[j].stride,&rate)==3);assert(rate<=1);bindings[j].inputRate=(VkVertexInputRate)rate;}
        vi.vertexBindingDescriptionCount=nb;vi.pVertexBindingDescriptions=bindings;vi.vertexAttributeDescriptionCount=na;vi.pVertexAttributeDescriptions=attrs;
        VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,NULL,0,2,dsl,0,NULL};VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(dev,&li,NULL,&layout));
        VkShaderModule modules[2]={module(root,vp),module(root,fp)};
        VkPipelineShaderStageCreateInfo ss[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_VERTEX_BIT,modules[0],"main",NULL},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_FRAGMENT_BIT,modules[1],"main",NULL}};
        VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,NULL,0,(VkPrimitiveTopology)topo,VK_FALSE};
        VkPipelineViewportStateCreateInfo vs={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,NULL,0,1,NULL,1,NULL};
        VkPipelineRasterizationStateCreateInfo rs={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,NULL,0,VK_FALSE,VK_FALSE,VK_POLYGON_MODE_FILL,VK_CULL_MODE_BACK_BIT,VK_FRONT_FACE_CLOCKWISE,VK_FALSE,0,0,0,1};
        VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,NULL,0,(VkSampleCountFlagBits)samples,VK_FALSE,0,NULL,VK_FALSE,VK_FALSE};
        VkPipelineDepthStencilStateCreateInfo ds={VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthTestEnable=dt;ds.depthWriteEnable=dw;ds.depthCompareOp=(VkCompareOp)df;ds.maxDepthBounds=1;
        VkPipelineColorBlendAttachmentState ba[8]={0};VkFormat formats[8]={0};for(unsigned j=0;j<nc;j++){unsigned fmt,mask;assert(fscanf(f,"%u %u",&fmt,&mask)==2 && mask<=15);formats[j]=(VkFormat)fmt;ba[j].colorWriteMask=mask;}
        VkPipelineColorBlendStateCreateInfo cb={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,NULL,0,VK_FALSE,VK_LOGIC_OP_COPY,nc,ba,{0}};
        VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,VK_DYNAMIC_STATE_STENCIL_REFERENCE,VK_DYNAMIC_STATE_DEPTH_BIAS,VK_DYNAMIC_STATE_LINE_WIDTH};
        VkPipelineDynamicStateCreateInfo dy={VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,NULL,0,8,states};
        VkPipelineRenderingCreateInfo render={VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,NULL,0,nc,formats,(VkFormat)dfmt,VK_FORMAT_UNDEFINED};
        VkGraphicsPipelineCreateInfo ci={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,&render,VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR|VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR,
            2,ss,&vi,&ia,NULL,&vs,&rs,&ms,&ds,&cb,&dy,layout,VK_NULL_HANDLE,0,VK_NULL_HANDLE,-1};
        VkPipeline pipe;CHECK(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&ci,NULL,&pipe));dump(root,label,pipe);printf("COMPILED %s\n",label);fflush(stdout);
        vkDestroyPipeline(dev,pipe,NULL);for(unsigned j=0;j<2;j++){vkDestroyShaderModule(dev,modules[j],NULL);vkDestroyDescriptorSetLayout(dev,dsl[j],NULL);}vkDestroyPipelineLayout(dev,layout,NULL);
    }fclose(f);
}

int main(int argc,char **argv) {
    assert(argc==4);
    char path[1024];snprintf(path,sizeof path,"%s/%s",argv[1],argv[2]);
    void *lib=dlopen(path,RTLD_NOW|RTLD_LOCAL);NEED(lib);
    gip=(PFN_vkGetInstanceProcAddr)dlsym(lib,"vk_icdGetInstanceProcAddr");
    if(!gip)gip=(PFN_vkGetInstanceProcAddr)dlsym(lib,"vkGetInstanceProcAddr");
    if(!gip) {
        // Android HAL-only drivers export HMI rather than an ICD entry point.
        // Use the same existing adrenotools loader path as the game runtime.
        snprintf(path,sizeof path,"%s/libadrenotools.so",argv[1]);
        void *at=dlopen(path,RTLD_NOW|RTLD_LOCAL);NEED(at);
        void *(*open_vk)(int,int,const char*,const char*,const char*,const char*,const char*,void**)=(void*)dlsym(at,"adrenotools_open_libvulkan");NEED(open_vk);
        char dirs[1024];snprintf(dirs,sizeof dirs,"%s/",argv[1]);
        lib=open_vk(RTLD_NOW|RTLD_LOCAL,1,NULL,argv[1],dirs,argv[2],NULL,NULL);NEED(lib);
        gip=(PFN_vkGetInstanceProcAddr)dlsym(lib,"vkGetInstanceProcAddr");
    }
    NEED(gip);
    LOAD_I(vkCreateInstance);
    VkApplicationInfo app={VK_STRUCTURE_TYPE_APPLICATION_INFO,NULL,"nx2a-compile-probe",1,"inspection-only",1,VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,NULL,0,&app,0,NULL,0,NULL};
    CHECK(vkCreateInstance(&ici,NULL,&inst));
    LOAD_I(vkEnumeratePhysicalDevices);LOAD_I(vkGetPhysicalDeviceProperties2);LOAD_I(vkGetPhysicalDeviceFeatures2);
    LOAD_I(vkGetPhysicalDeviceQueueFamilyProperties);LOAD_I(vkEnumerateDeviceExtensionProperties);LOAD_I(vkCreateDevice);LOAD_I(vkDestroyInstance);
    uint32_t npd=8;VkPhysicalDevice pds[8];CHECK(vkEnumeratePhysicalDevices(inst,&npd,pds));assert(npd>0 && npd<=8);
    VkPhysicalDevice pd=pds[0];
    VkPhysicalDeviceDriverProperties driver={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&driver};vkGetPhysicalDeviceProperties2(pd,&props);
    printf("DEVICE %s driver %s info %s api %#x\n",props.properties.deviceName,driver.driverName,driver.driverInfo,props.properties.apiVersion);
    // Drivers exposing the requested executable extension are accepted;no vendor assumption.
    uint32_t ne=0;CHECK(vkEnumerateDeviceExtensionProperties(pd,NULL,&ne,NULL));assert(ne<=1024);
    VkExtensionProperties *ext=calloc(ne,sizeof *ext);assert(ext);CHECK(vkEnumerateDeviceExtensionProperties(pd,NULL,&ne,ext));
    int found=0;for(uint32_t i=0;i<ne;i++)if(!strcmp(ext[i].extensionName,VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME))found=1;
    assert(found);free(ext);
    uint32_t nq=0;vkGetPhysicalDeviceQueueFamilyProperties(pd,&nq,NULL);assert(nq>0 && nq<=32);
    VkQueueFamilyProperties qp[32];vkGetPhysicalDeviceQueueFamilyProperties(pd,&nq,qp);
    uint32_t q=0;while(q<nq && !(qp[q].queueFlags&VK_QUEUE_GRAPHICS_BIT))q++;assert(q<nq);
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pe={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features f13={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,&pe};
    VkPhysicalDeviceVulkan12Features f12={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,&f13};
    VkPhysicalDeviceFeatures2 f={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&f12};vkGetPhysicalDeviceFeatures2(pd,&f);
    assert(pe.pipelineExecutableInfo && f13.dynamicRendering);f.features.robustBufferAccess=0;f.features.shaderFloat64=0;
    f12.bufferDeviceAddressCaptureReplay=0;f12.bufferDeviceAddressMultiDevice=0;f13.robustImageAccess=0;
    const char *extensions[]={VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME};
    float priority=0;VkDeviceQueueCreateInfo qi={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,NULL,0,q,1,&priority};
    VkDeviceCreateInfo di={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&f,0,1,&qi,0,NULL,1,extensions,NULL};
    CHECK(vkCreateDevice(pd,&di,NULL,&dev));gdp=(PFN_vkGetDeviceProcAddr)gip(inst,"vkGetDeviceProcAddr");NEED(gdp);
    LOAD_D(vkDestroyDevice);
    printf("READY executable diagnostics; no command pool,draw or submission\n");fflush(stdout);
    compile_cases(argv[1],argv[3]);
    vkDestroyDevice(dev,NULL);vkDestroyInstance(inst,NULL);
    return 0;
}
