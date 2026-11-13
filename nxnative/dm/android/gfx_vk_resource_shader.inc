// nxnative 1f01e28c descriptor_tables.inc and uniform_table.inc, adapted to DMSV.
#include "spv_descriptor_table.h"
static int resource_pool_binding(const dmsv_bind *b) { return b->kind==RK_TEX_POOL || b->kind==RK_SAMP_POOL; }
static int resource_pool_spv(vkshader_t *s,uint32_t **words,uint32_t *nw) {
    unsigned pools=0,bounds=0,bounds_slots=0; uint32_t next=0; uint8_t slots[96]={0};
    for (int i=0;i<s->nbind;i++) {
        const dmsv_bind *b=&s->b[i];
        if (b->binding>=next) next=(uint32_t)b->binding+1;
        if (resource_pool_binding(b)) {
            if (b->reg<0 || b->reg>=95 || slots[b->reg] || b->count!=1 ||
                b->desc!=(b->kind==RK_TEX_POOL?RD_IMAGE:RD_SAMPLER) || b->off!=(b->kind==RK_TEX_POOL?1<<20:1<<12)) return 0;
            slots[b->reg]=1;pools++;
        }
        if (b->kind==RK_POOL_BOUNDS) { bounds++;bounds_slots=(unsigned)b->reg;if(b->desc!=RD_UBO || b->count!=1)return 0; }
    }
    if (!pools) return !bounds;
    if (!V.has_resource_pools || bounds!=1 || bounds_slots!=pools || s->nbind+(int)pools>96 || next+pools>96) return 0;
    for (unsigned i=0;i<pools;i++) if(!slots[i])return 0;
    dmsv_bind *bindings=malloc((s->nbind+pools)*sizeof *bindings);if(!bindings)return 0;
    memcpy(bindings,s->b,s->nbind*sizeof *bindings);
    descriptor_table_edit edits[96];unsigned count=0;int total=s->nbind;
    for(int i=0;i<s->nbind;i++)if(resource_pool_binding(&s->b[i])) {
        bindings[total++]=(dmsv_bind){.kind=RK_POOL_MAP,.desc=RD_SSBO,.binding=(uint16_t)next,.count=1,.reg=s->b[i].reg};
        edits[count++]=(descriptor_table_edit){s->b[i].binding,next++,0};
    }
    size_t out_count;const char *reason;uint32_t *out=spv_descriptor_table(*words,*nw,edits,count,&out_count,&reason);
    if(!out){VKLOG("[RED] shader %016llx descriptor pool remap: %s",(unsigned long long)s->key,reason);free(bindings);return 0;}
    free(*words);*words=out;*nw=(uint32_t)out_count;s->b=bindings;s->nbind=total;s->has_pools=1;return vk_shader_pool_limits(&s,1);
}
// Same per-stage and combined-layout budget checks as donor table_descriptor_limits.
int vk_shader_pool_limits(vkshader_t *const *stages,int n) {
    const VkPhysicalDeviceLimits *l=&V.props.limits;uint64_t ai=0,as=0,au=0,ab=0,ast=0;
    for(int st=0;st<n;st++) {
        const vkshader_t *s=stages[st];if(!s)continue;
        uint64_t images=0,samplers=0,uniforms=0,buffers=0,storage=0,resources=0;
        for(int i=0;i<s->nbind;i++) {
            const dmsv_bind *b=&s->b[i];unsigned c=b->count?b->count:32;
            if(b->desc==RD_IMAGE || b->desc==RD_SAMPLED || b->desc==RD_TBUF)images+=c;
            if(b->desc==RD_SAMPLER || b->desc==RD_SAMPLED)samplers+=c;
            if(b->desc==RD_UBO)uniforms+=c;if(b->desc==RD_SSBO)buffers+=c;
            if(b->desc==RD_STORAGE || b->desc==RD_STORAGE_TBUF)storage+=c;
            if(b->desc!=RD_SAMPLER)resources+=c;
        }
        if(images>l->maxPerStageDescriptorSampledImages || samplers>l->maxPerStageDescriptorSamplers ||
           uniforms>l->maxPerStageDescriptorUniformBuffers || buffers>l->maxPerStageDescriptorStorageBuffers ||
           storage>l->maxPerStageDescriptorStorageImages || resources>l->maxPerStageResources)return 0;
        ai+=images;as+=samplers;au+=uniforms;ab+=buffers;ast+=storage;
    }
    return ai<=l->maxDescriptorSetSampledImages && as<=l->maxDescriptorSetSamplers && au<=l->maxDescriptorSetUniformBuffers &&
           ab<=l->maxDescriptorSetStorageBuffers && ast<=l->maxDescriptorSetStorageImages;
}
vkshader_t *vk_shader_pool_variant(vkshader_t *selected,const uint32_t counts[96]) {
    vkshader_t *base=selected->pool_base?selected->pool_base:selected;
    if(!base->has_pools)return base;
    pthread_mutex_lock(&g_sh_mu);
    for(vkshader_t *s=base;s;s=s==base?base->pool_variants:s->pool_next) {
        int match=1;for(int i=0;i<s->nbind;i++)if(resource_pool_binding(&s->b[i]) && s->b[i].count!=counts[s->b[i].reg])match=0;
        if(match){pthread_mutex_unlock(&g_sh_mu);return s->dsl?s:NULL;}
    }
    vkshader_t *s=malloc(sizeof *s);dmsv_bind *b=malloc(base->nbind*sizeof *b);
    if(!s || !b){free(s);free(b);pthread_mutex_unlock(&g_sh_mu);return NULL;}
    *s=*base;memcpy(b,base->b,base->nbind*sizeof *b);s->b=b;s->pool_base=base;s->pool_variants=NULL;s->nvar=0;
    VkDescriptorSetLayoutBinding lb[96];int valid=1;
    for(int i=0;i<s->nbind;i++) {
        if(resource_pool_binding(&b[i])) {unsigned c=counts[b[i].reg];if(!c || c>UINT16_MAX)valid=0;else b[i].count=(uint16_t)c;}
        lb[i]=(VkDescriptorSetLayoutBinding){b[i].binding,s->dyn[i]?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:desc_type(b[i].desc),b[i].count?b[i].count:32,stage_flag(s->stage),NULL};
    }
    VkDescriptorSetLayoutSupport support={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
    VkDescriptorSetLayoutCreateInfo ci={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,NULL,0,(uint32_t)s->nbind,lb};
    if(valid && vk_shader_pool_limits(&s,1))vkGetDescriptorSetLayoutSupport(V.dev,&ci,&support);
    if(!support.supported){free(b);free(s);pthread_mutex_unlock(&g_sh_mu);return NULL;}
    s->dsl=shader_layout(lb,(uint32_t)s->nbind);
    static uint32_t variant; s->pool_variant=++variant;
    s->pool_next=base->pool_variants;base->pool_variants=s;
    pthread_mutex_unlock(&g_sh_mu);return s;
}
