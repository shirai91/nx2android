// nxnative 1f01e28c: prepare_tables, descriptor maps/bounds and chunked descriptor writes.
#include "gfx_vk_resource_pool.h"
static vk_resource_table *resource_tables[6][96];
static int prepare_resource_pools(int st,vkshader_t **selected) {
    vkshader_t *s=*selected;if(!s || !s->has_pools)return 1;
    uint32_t counts[96]={0};
    for(int i=0;i<s->nbind;i++) {
        const dmsv_bind *b=&s->b[i];if(b->kind!=RK_TEX_POOL && b->kind!=RK_SAMP_POOL)continue;
        vk_resource_table **entry=&resource_tables[st][b->reg];if(!*entry)*entry=calloc(1,sizeof **entry);
        int sampler=b->kind==RK_SAMP_POOL;
        if(!*entry || !vk_resource_pool_build(*entry,sampler,sampler?g_pool_samp_base:g_pool_tex_base))return 0;
        counts[b->reg]=(*entry)->count;
    }
    vkshader_t *variant=vk_shader_pool_variant(s,counts);
    if(!variant){VKRED_ONCE(97,"resource pool descriptor layout exceeds device limits");return 0;}
    *selected=variant;return 1;
}
// Donor tex_slot: incompatible dimensional entries use the same zero texture;
// raw 32-bit integer words are viewed as float for Maxwell's typeless registers.
static VkImageView resource_pool_view(const dmsv_bind *b,uint64_t handle) {
    VkImageView dummy=vk_dummy_view(b->dim,b->flags&1,!!(b->flags&2),0,0);
    const tex_t *t=NULL;const view_t *v=NULL;unsigned id=(unsigned)(handle&0xfffff);
    if(!id || !vk_tex_by_id((int)id,&v,&t) || !t || t->d.target==NVN_TEXTURE_TARGET_BUFFER)return dummy;
    vkimg_t *im=vk_tex(t);if(!im)return dummy;
    if(b->dim==0 || im->vtype==VK_IMAGE_VIEW_TYPE_1D || im->vtype==VK_IMAGE_VIEW_TYPE_1D_ARRAY ||
       (im->vtype==VK_IMAGE_VIEW_TYPE_3D)!=(b->dim==2) || !!(b->flags&2)!=(im->samples>1) ||
       (b->dim==3 && (im->layers<6 || im->w!=im->h || im->samples!=VK_SAMPLE_COUNT_1_BIT)))return dummy;
    if(v && v->set_target && (v->target==NVN_TEXTURE_TARGET_1D || v->target==NVN_TEXTURE_TARGET_1D_ARRAY ||
       (b->dim==2 && v->target!=NVN_TEXTURE_TARGET_3D)))return dummy;
    NVNformat fit=NVN_FORMAT_NONE;
    const vkfmt_t *f=vk_fmt(v && v->set_format?v->format:t->d.format);
    if(f)switch(f->vk) {
        case VK_FORMAT_R32_UINT:case VK_FORMAT_R32_SINT:fit=NVN_FORMAT_R32F;break;
        case VK_FORMAT_R32G32_UINT:case VK_FORMAT_R32G32_SINT:fit=NVN_FORMAT_RG32F;break;
        case VK_FORMAT_R32G32B32A32_UINT:case VK_FORMAT_R32G32B32A32_SINT:fit=NVN_FORMAT_RGBA32F;break;
        default:break;
    }
    if(f && f->is_int && !fit) { VKRED_ONCE(98,"resource pool integer format requires a separate typed-view port");return dummy; }
    if(!f)return dummy;
    // Donor tex_slot changes the sampled view's NVN format, retaining both
    // texture and explicit view swizzles. The storage override erases them.
    view_t adjusted=v?*v:(view_t){0};
    if(fit){adjusted.set_format=1;adjusted.format=(uint16_t)fit;}
    VkFormat expected=fit?vk_fmt(fit)->vk:f->vk;
    VkImageView view=vk_tex_view(t,&adjusted,b->dim,b->flags&1,0,&im);
    // A format-listed allocation can refuse reinterpretation and return its
    // original integer format. It cannot be sampled by this float interface.
    if(view && vk_view_fmt!=expected)return dummy;
    if(im){im->last_use=mtl_last_fence()+1;vk_texdump_note(im);}
    return view?view:dummy;
}
static void write_resource_pool(VkDescriptorSet set,int st,const dmsv_bind *b) {
    const vk_resource_table *table=resource_tables[st][b->reg];
    for(uint32_t first=0;first<b->count;first+=32) {
        uint32_t count=b->count-first<32?b->count-first:32;VkDescriptorImageInfo images[32]={0};
        for(uint32_t i=0;i<count;i++) {
            uint64_t handle=table->handles[first+i];
            if(b->kind==RK_SAMP_POOL) {VkSampler sampler=vk_samp_by_id((int)(handle>>20));images[i].sampler=sampler?sampler:vk_dummy_sampler();}
            else {images[i].imageView=resource_pool_view(b,handle);images[i].imageLayout=VK_IMAGE_LAYOUT_GENERAL;}
        }
        VkWriteDescriptorSet w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,NULL,set,b->binding,first,count,
            b->kind==RK_SAMP_POOL?VK_DESCRIPTOR_TYPE_SAMPLER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,images,NULL,NULL};
        vkUpdateDescriptorSets(V.dev,1,&w,0,NULL);
    }
}
