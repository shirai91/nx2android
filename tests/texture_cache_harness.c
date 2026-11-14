// Execute the production cache lookup, upload decisions, eviction and pool drop.
// The GPU boundary stores one representative byte; this is not a Vulkan device test.
#include "nvn_obj.h"
#include "nvn_hmap.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define VKLOG(...) ((void)0)
#define VKRED_ONCE(...) ((void)0)
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define pthread_mutex_lock(p) ((void)(p))
#define pthread_mutex_unlock(p) ((void)(p))
typedef struct { int depth,stencil,vk; } vkfmt_t;
typedef struct vkimg {
    uint64_t pool,used,pkg_used,pkg_vgen,pkg_stamp,pkg_seen,pkg_hash;
    int64_t off,size; int flags,looked,packaged,pkg_sparse,pkg_released,gpu_w;
    const void *pkg_span,*pkg_mem,*lin_mem;size_t pkg_len,lin_len;
    void *img,*mem,*sub;
    struct vkimg *bc_draw; // No compressed-format stand-in in this family fixture.
    unsigned family_formats,family_closed;
    int format,content;
} vkimg_t;
static unsigned written,uploads,created,dropped;
static unsigned char retired[4096];
static uint64_t g_tex_frame;
static const vkfmt_t *vk_fmt(int format) {static vkfmt_t f;f=(vkfmt_t){.vk=format};return &f;}
static vkimg_t *img_new(const tex_t *t,const vkfmt_t *f) {(void)t;vkimg_t *im=calloc(1,sizeof *im);assert(im);im->format=f->vk;im->img=(void*)(uintptr_t)++created;assert(created<sizeof retired);return im;}
static void alias_link(vkimg_t *im) {(void)im;}
static void alias_unlink(vkimg_t *im) {(void)im;}
static void vk_lrz_forget(vkimg_t *im) {(void)im;}
static void vk_tex_view_drop_all(vkimg_t *im) {(void)im;}
static void nvn_dirty_release(const void *mem,size_t len) {(void)mem;(void)len;}
static void vk_defer_free_image(void *img,void *mem) {(void)mem;unsigned n=(unsigned)(uintptr_t)img;assert(n && n<=created && !retired[n]);retired[n]=1;dropped++;}
static void vk_defer_free_image_sub(void *img,void *sub) {(void)sub;vk_defer_free_image(img,NULL);}
static int vk_sub_open(void) {return 1;}
static int vk_upload_cmd(void) {return 1;}
static void vk_tex_prepare(int cb,vkimg_t *im) {(void)cb;im->content=0;}
static int lin_copy(vkimg_t *im,const tex_t *t,const vkfmt_t *f) {(void)im;(void)t;(void)f;return 0;}
static int packaged_tex(const tex_t *t) {return t->has_pkg_data || t->has_layout || (written&(1u<<(t->serial-1)));}
int nvn_ft_written(const tex_t *t) {(void)t;return 0;}
static int cpu_content_tex(const tex_t *t,const vkfmt_t *f) {(void)t;(void)f;return 0;}
static uint64_t nvn_dirty_now(void) {return 1;}
static uint64_t pkg_hash(const uint8_t *mem,size_t size) {(void)size;return mem[0];}
static void pkg_upload(vkimg_t *im,const tex_t *t,const vkfmt_t *f) {
    (void)f;uploads++;im->content=t->mem[0];im->pkg_seen=im->pkg_stamp=1;
    im->pkg_hash=pkg_hash(t->mem,t->L.size);im->pkg_mem=t->mem;im->pkg_span=t->mem;im->pkg_len=t->L.size;
}
static int nvn_sparse_since(const void *pool,uint64_t addr,size_t size,uint64_t gen,uint64_t stamp) {(void)pool;(void)addr;(void)size;(void)gen;(void)stamp;return 0;}
static int nvn_dirty_since_span(const void *span,const void *mem,size_t size,uint64_t stamp) {(void)span;(void)mem;(void)size;(void)stamp;return 0;}
static void nvn_dirty_protect(const void *mem,size_t size) {(void)mem;(void)size;}
/* CACHE KEYS */
/* CACHE LIFETIME */
/* LOOKUP */
/* DROP POOL */
static uint8_t storage[4096]={0x83};
static pool_t pool={.mem=storage};
static tex_t base_tex(int format,unsigned serial) {
    return (tex_t){.magic=MAGIC('T','E','X',0),.serial=serial,.pool=&pool,.mem=storage,.L={.size=sizeof storage},
                  .d={.width=8,.height=8,.depth=1,.levels=1,.target=NVN_TEXTURE_TARGET_2D,.format=format}};
}
static void reset(void) {
    vk_tex_drop_pool(&pool);
    for(unsigned i=0;i<g_by_serial.cap;i++)if(g_by_serial.vals[i])img_drop(g_by_serial.vals[i]);
    nvn_hm_clear(&g_by_serial,NULL);assert(!g_img_n && !g_by_key.n);written=0;g_tex_frame=0;
}
static int same(tex_t a,tex_t b) {reset();vkimg_t *x=vk_tex_(&a),*y=vk_tex_(&b);assert(x && y);return x==y;}
int main(void) {
    const int families[2][4]={{NVN_FORMAT_RGBA8,NVN_FORMAT_RGBA8_SRGB,NVN_FORMAT_RGBX8,NVN_FORMAT_RGBX8_SRGB},
                             {NVN_FORMAT_BGRA8,NVN_FORMAT_BGRA8_SRGB,NVN_FORMAT_BGRX8,NVN_FORMAT_BGRX8_SRGB}};
    for(int family=0;family<2;family++)for(int array=0;array<2;array++)for(int i=0;i<4;i++)for(int j=0;j<4;j++) {
        tex_t a=base_tex(families[family][i],1),b=base_tex(families[family][j],2);
        a.d.target=b.d.target=array?NVN_TEXTURE_TARGET_2D_ARRAY:NVN_TEXTURE_TARGET_2D;assert(same(a,b));
    }
    // Metadata peers keep main's equal-description sharing in both orders.
    for(int family=0;family<2;family++)for(int f=0;f<4;f++)for(int layout=0;layout<2;layout++)for(int order=0;order<2;order++) {
        reset();tex_t plain=base_tex(families[family][f],1),pkg=base_tex(families[family][f],2);
        pkg.has_pkg_data=!layout;pkg.has_layout=layout;
        tex_t *first=order?&plain:&pkg,*second=order?&pkg:&plain;
        vkimg_t *a=vk_tex_(first);assert(a && a->content==(order?0:0x83));
        vkimg_t *b=vk_tex_(second);assert(a==b && a->content==0x83 && g_img_n==1 && g_by_key.n==1);
        a->content=0x45;a->gpu_w=1;assert(vk_tex_(second)->content==0x45);
        b->content=0x7b;assert(vk_tex_(first)->content==0x7b);
        tex_t alternate=base_tex(families[family][(f+1)%4],3);
        assert(vk_tex_(&alternate)!=a); // metadata must not admit a new format
    }
    // All 24 orders: exact peers win even when another family image exists.
    // Cross-format sharing survives only when both plain peers met first.
    for(int family=0;family<2;family++)for(int layout=0;layout<2;layout++)
    for(int i=0;i<4;i++)for(int j=0;j<4;j++)for(int k=0;k<4;k++)for(int l=0;l<4;l++) {
        if(i==j || i==k || i==l || j==k || j==l || k==l)continue;
        reset();tex_t peers[4];vkimg_t *found[4]={0};int order[]={i,j,k,l};
        for(int n=0;n<4;n++){peers[n]=base_tex(families[family][n%2],n+1);if(n>=2){peers[n].has_pkg_data=!layout;peers[n].has_layout=layout;}}
        for(int n=0;n<4;n++) {
            int id=order[n];found[id]=vk_tex_(&peers[id]);assert(found[id]);
            for(int q=0;q<=n;q++)assert(vk_tex_(&peers[order[q]])==found[order[q]]);
        }
        assert(found[0]==found[2] && found[1]==found[3]);
        assert((found[0]==found[1])==(i<2 && j<2));
        assert(g_by_key.n==(i<2 && j<2?1:2) && g_img_n==g_by_key.n);
        found[0]->content=0x29;found[0]->gpu_w=1;assert(vk_tex_(&peers[2])->content==0x29);
        found[1]->content=0x62;found[1]->gpu_w=1;assert(vk_tex_(&peers[3])->content==0x62);
    }
    reset();tex_t a=base_tex(NVN_FORMAT_RGBA8,1),b=base_tex(NVN_FORMAT_RGBA8_SRGB,2);
    vkimg_t *image=vk_tex_(&a);written=1;assert(vk_tex_(&a)==image && image->content==0x83);
    b.d.format=a.d.format;assert(vk_tex_(&b)==image && g_img_n==1);
    // An established family cannot split when metadata peers arrive later.
    reset();b.d.format=NVN_FORMAT_RGBA8_SRGB;image=vk_tex_(&a);assert(vk_tex_(&b)==image);
    tex_t pa=a,pb=b;pa.serial=3;pb.serial=4;pa.has_pkg_data=pb.has_pkg_data=1;
    assert(vk_tex_(&pa)==image && vk_tex_(&pb)==image);
    tex_t x=base_tex(NVN_FORMAT_RGBX8,5);vkimg_t *other=vk_tex_(&x);assert(other!=image);
    x.has_layout=1;assert(vk_tex_(&x)==other && g_img_n==2 && g_by_key.n==2);
    // Every image has one owner: eviction and pool deletion each retire it once.
    unsigned before=dropped;g_tex_frame=61;evict_overlaps((uint64_t)(uintptr_t)&pool,0,sizeof storage);
    assert(g_img_n==0 && g_by_key.n==0 && dropped==before+2);
    image=vk_tex_(&pa);assert(vk_tex_(&a)==image);assert(vk_tex_(&b)!=image);
    before=dropped;vk_tex_drop_pool(&pool);assert(g_img_n==0 && g_by_key.n==0 && dropped==before+2);
    // A real differing layout remains different even with equal formats.
    b=a;b.serial=2;b.has_layout=1;b.d.pkg_block_h=2;assert(!same(a,b));a.d.pkg_block_h=2;assert(same(a,b));
    a=base_tex(NVN_FORMAT_RGBA8,1);b=base_tex(NVN_FORMAT_RGBA8_SRGB,2);
    for(int mask=1;mask<=3;mask++){a.has_pkg_data=mask&1;b.has_pkg_data=(mask>>1)&1;assert(!same(a,b));}a.has_pkg_data=b.has_pkg_data=0;
    a.d.flags=b.d.flags=NVN_TEXTURE_FLAGS_LINEAR;assert(!same(a,b));a.d.flags=b.d.flags=0;
    b.d.flags=NVN_TEXTURE_FLAGS_IMAGE;assert(!same(a,b));b.d.flags=0;
    b.d.width=16;assert(!same(a,b));b.d.width=8;
    b.d.format=NVN_FORMAT_BGRA8;assert(!same(a,b));b.d.format=NVN_FORMAT_RGBA8_SRGB;
    a.has_layout=b.has_layout=1;assert(!same(a,b));a.has_layout=b.has_layout=0;
    pool.flags=NVN_MEMORY_POOL_FLAGS_VIRTUAL;assert(!same(a,b));pool.flags=0;
    a.mem=b.mem=NULL;assert(!same(a,b));a.mem=b.mem=storage;
    a.pool=b.pool=NULL;assert(!same(a,b));a.pool=b.pool=&pool;
    a.d.samples=b.d.samples=4;assert(!same(a,b));a.d.samples=b.d.samples=0;
    a.d.target=b.d.target=NVN_TEXTURE_TARGET_3D;assert(!same(a,b));
    a.d.target=b.d.target=NVN_TEXTURE_TARGET_CUBEMAP;assert(!same(a,b));
    reset();free(g_by_key.keys);free(g_by_key.vals);free(g_by_serial.keys);free(g_by_serial.vals);
    assert(created==dropped);puts("family pairs, metadata peers, upload visibility, closure and unique retirement passed");
}
