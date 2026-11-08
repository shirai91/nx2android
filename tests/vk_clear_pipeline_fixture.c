// Exercise the production cache with a driver rejecting one pipeline key.
#include <vulkan/vulkan_core.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "nvn_hmap.h"
static struct { VkDevice dev; VkPipelineCache pcache; int has_dynrender; } V={.has_dynrender=1};
static const uint32_t maskclear_vert_spv[]={0}, maskclear_frag_spv[]={0}, maskclear_uint_frag_spv[]={0}, maskclear_sint_frag_spv[]={0};
static unsigned attempts;
#define VKRED_ONCE(...) ((void)0)
static int rp_on(void) {return 0;}
static VkRenderPass rp_compat(uint32_t n,const uint32_t *f,uint32_t d,uint32_t s) {(void)n;(void)f;(void)d;(void)s;return VK_NULL_HANDLE;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice d,const VkPipelineLayoutCreateInfo *i,const VkAllocationCallbacks *a,VkPipelineLayout *p) {(void)d;(void)i;(void)a;*p=(VkPipelineLayout)(uintptr_t)2;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice d,const VkShaderModuleCreateInfo *i,const VkAllocationCallbacks *a,VkShaderModule *p) {(void)d;(void)i;(void)a;*p=(VkShaderModule)(uintptr_t)2;return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice d,VkPipelineCache c,uint32_t n,const VkGraphicsPipelineCreateInfo *i,const VkAllocationCallbacks *a,VkPipeline *p) {
    (void)d;(void)c;(void)a;assert(n==1);attempts++;
    if(i->pColorBlendState->pAttachments[0].colorWriteMask==1){*p=VK_NULL_HANDLE;return VK_ERROR_FORMAT_NOT_SUPPORTED;}
    *p=(VkPipeline)(uintptr_t)2;return VK_SUCCESS;
}
/* PRODUCTION_PIPELINE */
int main(void) {
    mckey key={.ncol=1,.cfmt={VK_FORMAT_R8G8B8A8_UNORM},.samples=1,.mask=1};
    assert(!mc_pipeline_key(&key));assert(!mc_pipeline_key(&key));assert(attempts==1);
    key.mask=2;assert(mc_pipeline_key(&key)==(VkPipeline)(uintptr_t)2);assert(mc_pipeline_key(&key)==(VkPipeline)(uintptr_t)2);assert(attempts==2);
    free(g_mc_pipes.keys);free(g_mc_pipes.vals);return 0;
}
