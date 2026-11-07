"""Texture refresh must retain concurrent and untracked CPU writes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from porter.toolchain import find_ndk


class TextureDirty(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_write_during_protection(self):
        self._check_texture_writes()

    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_real_page_fault_during_upload(self):
        self._check_texture_writes(real_pages=True)

    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_failed_upload_is_retried(self):
        self._check_texture_writes(failures=True)

    @unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
    def test_real_write_after_gpu_owned_texture_hash(self):
        self._check_texture_writes(real_pages=True, keep_race=True)

    def _check_texture_writes(self, real_pages=False, failures=False, keep_race=False):
        ndk, _ = find_ndk()
        headers = next(ndk.glob('toolchains/llvm/prebuilt/*/sysroot/usr/include'), None) if ndk else None
        if not headers:
            self.skipTest('NDK Vulkan headers are required')
        src = (Path(__file__).resolve().parents[1] / 'nxnative/dm/android/gfx_vk_tex.c').read_text()

        def function(signature):
            start = src.index(signature)
            return src[start:src.index('\n}\n', start) + 3]

        functions = '\n'.join(function(s) for s in (
            'static uint64_t pkg_hash(const uint8_t *m, size_t n) {',
            'static void pkg_upload_(vkimg_t *im, const tex_t *t, const vkfmt_t *f) {',
            'static int lin_copy(vkimg_t *im, const tex_t *t, const vkfmt_t *f) {'))
        start = src.index('            uint64_t now = nvn_dirty_now();', src.index('static vkimg_t *vk_tex_(const tex_t *t) {'))
        end = src.index('            if (!im->pkg_pending) im->pkg_seen = now;', start) + len('            if (!im->pkg_pending) im->pkg_seen = now;')
        refresh = src[start:end]
        harness = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NVN_TEXTURE_FLAGS_LINEAR 1
#define NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET 2
#define VKLOG(...) fprintf(stderr, __VA_ARGS__)
#define VKRED_ONCE(...) do { assert(expected_error); errors++; } while(0)
typedef struct { unsigned width,height,depth,format,target,flags,stride; int pkg_block_h; } desc;
typedef struct { uint64_t serial; uint8_t *mem; void *pool; desc d; struct { uint64_t size; int tileH; } L; } tex_t;
typedef struct { unsigned bw,bh,bpb,dbpp; int dec; VkFormat vk; } vkfmt_t;
typedef struct {
    VkImage img; VkImageLayout layout; VkImageAspectFlags aspect;
    unsigned w,h,d,levels,layers; int packaged,pkg_released,gpu_w,pkg_pending;
    uint64_t pkg_stamp,pkg_seen,lin_stamp,lin_gen,pkg_hash;
    const void *pkg_span,*lin_span; const uint8_t *pkg_mem,*lin_mem; size_t pkg_len,lin_len;
} vkimg_t;
static uint8_t memory[4], scratch[4], image[4];
static uint64_t now=10, dirty=10, g_up_gen=1;
static int write_pending, inject_write, copies, releases, tracked=1, queries;
static int expected_error,errors,fail_alloc,decode_failure,decode_calls;
static uint64_t nvn_dirty_now(void) { return now; }
static void nvn_dirty_protect(const void *p, size_t n) {
    assert(p==memory && n==sizeof memory);
    if (!tracked) return;
    // The writer faults after protection, stamps its page, then is descheduled
    // before storing bytes. It resumes only after this texture was uploaded.
    if (inject_write) { inject_write=0; dirty=++now; write_pending=1; }
}
static const void *nvn_dirty_span(const void *p, size_t n) { assert(p==memory && n==sizeof memory); return tracked ? memory : NULL; }
static int nvn_dirty_since_span(const void *span, const void *p, size_t n, uint64_t stamp) {
    assert((span==memory || !tracked) && p==memory && n==sizeof memory);
    queries++; return !tracked || dirty>stamp;
}
static void nvn_dirty_release(const void *p, size_t n) { assert(p==memory && n==sizeof memory); releases++; }
static int linear_storage(const tex_t *t) { return t->pool && (t->d.flags & 3); }
static VkCommandBuffer vk_upload_cmd(void) { return (VkCommandBuffer)(uintptr_t)1; }
static void vk_tex_prepare(VkCommandBuffer cb, vkimg_t *im) { assert(cb); im->layout=VK_IMAGE_LAYOUT_GENERAL; }
static void vk_img_barrier_all(VkCommandBuffer cb) { assert(cb); }
static void *vk_scratch(size_t n, size_t align, VkBuffer *b, VkDeviceSize *o) {
    assert(n<=sizeof scratch && align==16); *b=(VkBuffer)(uintptr_t)1; *o=0; return scratch;
}
static int vk_buf_at(uint64_t a, uint64_t n, VkBuffer *b, VkDeviceSize *o) {
    assert(a==(uintptr_t)memory && n==sizeof memory); *b=(VkBuffer)(uintptr_t)2; *o=0; return 1;
}
static int nvn_deswizzle(const desc *d, unsigned lv, unsigned ly, const void *src, void *dst, size_t pitch) {
    (void)d; assert(!ly && pitch==(size_t)(sizeof memory >> lv)); decode_calls++;
    if(decode_calls==decode_failure)return -1;
    memcpy(dst,src,pitch); return 0;
}
static void bc_decode(const vkfmt_t *f, const void *src, size_t pitch, unsigned w, unsigned h, unsigned z, void *dst) {
    assert(f->dec==1 && pitch==w && h==1 && z==1);memcpy(dst,src,pitch);
}
static void vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage im, VkImageLayout layout, uint32_t n, const VkBufferImageCopy *r) {
    assert(cb && im && layout==VK_IMAGE_LAYOUT_GENERAL && n==1);
    assert(!r->bufferOffset && r->imageExtent.width==(4u >> r->imageSubresource.mipLevel));
    memcpy(image,b==(VkBuffer)(uintptr_t)1 ? scratch : memory,r->imageExtent.width); copies++;
}
static void *upload_malloc(size_t n) {
    if(fail_alloc) { fail_alloc=0;return NULL; }return malloc(n);
}
#define malloc upload_malloc
FUNCTIONS
static void pkg_upload(vkimg_t *im, const tex_t *t, const vkfmt_t *f) { pkg_upload_(im,t,f); }
static void refresh(vkimg_t *im, const tex_t *t, const vkfmt_t *f) {
REFRESH
}
static void finish_write(void) { assert(write_pending); memory[0]=29; write_pending=0; }
int main(int argc, char **argv) {
    assert(argc==2); memset(memory,7,sizeof memory);
    tex_t t={.serial=1,.mem=memory,.pool=memory,.d={.width=4,.height=1,.depth=1},.L={.size=4}};
    vkfmt_t f={.bw=1,.bh=1,.bpb=1};
    vkimg_t im={.img=(VkImage)(uintptr_t)1,.layout=VK_IMAGE_LAYOUT_GENERAL,.aspect=VK_IMAGE_ASPECT_COLOR_BIT,.w=4,.h=1,.d=1,.levels=1,.layers=1};
    inject_write=1;
    if (atoi(argv[1])==0) {
        pkg_upload_(&im,&t,&f); assert(copies==1 && image[0]==7); finish_write();
        assert(nvn_dirty_now()!=im.pkg_seen && nvn_dirty_since_span(im.pkg_span,t.mem,t.L.size,im.pkg_stamp));
        pkg_upload_(&im,&t,&f); assert(copies==2 && image[0]==29);
        // No extra upload is needed once the changed bytes have been consumed.
        assert(nvn_dirty_now()==im.pkg_seen && !nvn_dirty_since_span(im.pkg_span,t.mem,t.L.size,im.pkg_stamp));
    } else if (atoi(argv[1])==1) {
        t.d.flags=NVN_TEXTURE_FLAGS_LINEAR; im.gpu_w=1;
        assert(lin_copy(&im,&t,&f) && !copies && im.gpu_w); finish_write();
        assert(lin_copy(&im,&t,&f) && copies==1 && !im.gpu_w && image[0]==29 && releases==1);
        assert(lin_copy(&im,&t,&f) && copies==1); // one copy per upload buffer
        im.gpu_w=1; im.lin_mem=NULL;
        assert(lin_copy(&im,&t,&f) && im.gpu_w);
        assert(lin_copy(&im,&t,&f) && im.gpu_w && copies==1); // no CPU write: preserve GPU ownership
    } else if (atoi(argv[1])==2) {
        tracked=0; inject_write=0;
        pkg_upload_(&im,&t,&f); assert(copies==1 && image[0]==7 && !im.pkg_span);
        uint64_t before=now;
        memory[0]=17; refresh(&im,&t,&f); assert(copies==2 && image[0]==17);
        memory[0]=31; refresh(&im,&t,&f); assert(copies==3 && image[0]==31);
        assert(now==before && queries==2); // untracked writes do not advance the global generation
        tracked=1; im.pkg_span=memory; int checked=queries;
        refresh(&im,&t,&f); assert(copies==3 && queries==checked); // tracked unchanged fast path stays
        // Upstream's GPU-owned texture guard must coexist with both local fixes.
        im.gpu_w=1; image[0]=77; dirty=++now;
        refresh(&im,&t,&f); assert(copies==3 && image[0]==77); // only adjacent bytes were dirty
        inject_write=1; dirty=++now;
        refresh(&im,&t,&f); assert(copies==3 && image[0]==77); finish_write();
        refresh(&im,&t,&f); assert(copies==4 && image[0]==29); // write started inside keep-path protection
    } else if (atoi(argv[1])==3) {
        inject_write=0;f.dec=1;f.dbpp=1;fail_alloc=1;
        pkg_upload_(&im,&t,&f);
        assert(im.pkg_pending && !copies && !im.pkg_stamp && !im.pkg_hash);
        assert(im.pkg_mem==memory && im.pkg_len==4 && !im.pkg_released);
        uint64_t generation=now;
        refresh(&im,&t,&f);assert(!im.pkg_pending && copies==1 && image[0]==7 && now==generation);
        refresh(&im,&t,&f);assert(copies==1);
    } else if (atoi(argv[1])==4) {
        inject_write=0;pkg_upload_(&im,&t,&f);assert(copies==1);
        uint64_t stamp=im.pkg_stamp,hash=im.pkg_hash,seen=im.pkg_seen;
        im.gpu_w=1;memory[0]=19;dirty=++now;im.levels=2;
        expected_error=1;decode_failure=decode_calls+2;
        refresh(&im,&t,&f);assert(errors==1 && copies==2 && im.pkg_pending);
        assert(im.pkg_stamp==stamp && im.pkg_hash==hash && im.pkg_seen==seen);
        // Matching the old hash must not hide an incomplete multi-mip upload.
        memory[0]=7;image[0]=77;uint64_t generation=now;
        refresh(&im,&t,&f);assert(!im.pkg_pending && copies==4 && image[0]==7 && now==generation);
        refresh(&im,&t,&f);assert(copies==4);
    } else {
        inject_write=0;expected_error=1;f.bpb=0;
        pkg_upload_(&im,&t,&f);assert(errors==1 && im.pkg_pending && !copies && !im.pkg_stamp);
        f.bpb=1;refresh(&im,&t,&f);assert(!im.pkg_pending && copies==1 && image[0]==7);
    }
}
'''.replace('FUNCTIONS', functions).replace('REFRESH', refresh)
        if real_pages:
            dirty = (Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/nvn/nvn_dirty.c').read_text()
            dirty = '\n'.join(line for line in dirty.splitlines() if not line.startswith('#include'))
            dirty = dirty.replace('void nvn_dirty_protect(', 'static void real_dirty_protect(')
            dirty = dirty.replace('void nvn_dirty_release(', 'static void real_dirty_release(')
            support = r'''
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
typedef struct { uint32_t numer,denom; } mach_timebase_info_data_t;
static void mach_timebase_info(mach_timebase_info_data_t *t) { t->numer=t->denom=1; }
static uint64_t mach_absolute_time(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
}
#define LOG(...) fprintf(stderr,__VA_ARGS__)
#define die(...) do { LOG(__VA_ARGS__);abort(); } while(0)
static int (*fault_hook)(void *,int,void *);
static void dm_fault_hook_register(int (*fn)(void *,int,void *)) { fault_hook=fn; }
static void dm_guest_write_prep_register(void (*fn)(void *,size_t)) { (void)fn; }
DIRTY
static int ready[2],resume[2]; static pthread_t writer;
static _Atomic int fault_count;
static void handle_fault(int sig,siginfo_t *info,void *context) {
    char token;
    if(sig!=SIGSEGV || atomic_fetch_add(&fault_count,1)>=8 ||
        !fault_hook(info->si_addr,1,context))_exit(90);
    // The real tracker has stamped/unprotected the page; postpone the actual store.
    if(write(ready[1],"F",1)!=1 || read(resume[0],&token,1)!=1 || token!='G')_exit(91);
}
static void *cpu_writer(void *unused) {
    (void)unused;((volatile uint8_t *)memory)[0]=29;
    assert(write(ready[1],"D",1)==1);return NULL;
}
static void nvn_dirty_protect(const void *addr,size_t len) {
    real_dirty_protect(addr,len);
    if(inject_write) {
        inject_write=0;write_pending=1;
        assert(!pthread_create(&writer,NULL,cpu_writer,NULL));
        char token;assert(read(ready[0],&token,1)==1 && token=='F');
    }
}
static void nvn_dirty_release(const void *addr,size_t len) {
    releases++;real_dirty_release(addr,len);
}
static void setup_pages(void) {
    assert(atomic_is_lock_free(&fault_count));
    assert(!pipe(ready) && !pipe(resume));
    size_t page=(size_t)getpagesize();
    memory=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(memory!=MAP_FAILED);nvn_dirty_add_range(memory,page);
    struct sigaction sa={.sa_sigaction=handle_fault,.sa_flags=SA_SIGINFO};
    sigemptyset(&sa.sa_mask);assert(!sigaction(SIGSEGV,&sa,NULL));
}
static void finish_real_write(void) {
    assert(write_pending && write(resume[1],"G",1)==1);
    char token;assert(read(ready[0],&token,1)==1 && token=='D');
    assert(!pthread_join(writer,NULL));write_pending=0;
}
static void cleanup_pages(void) {
    assert(!write_pending && atomic_load(&fault_count)==1);
    nvn_dirty_remove_range(memory);assert(!munmap(memory,(size_t)getpagesize()));
    assert(!close(ready[0]) && !close(ready[1]) && !close(resume[0]) && !close(resume[1]));
}
'''.replace('DIRTY', dirty)
            begin = harness.index('static uint64_t nvn_dirty_now(void)')
            end = harness.index('static int linear_storage(', begin)
            harness = harness[:begin] + support + harness[end:]
            harness = harness.replace('static uint8_t memory[4], scratch[4], image[4];',
                                      'static uint8_t *memory, scratch[4], image[4];')
            harness = harness.replace('sizeof memory', '4')
            harness = harness.replace('assert(argc==2);', 'assert(argc==2); setup_pages();')
            harness = harness.replace('assert(write_pending); memory[0]=29; write_pending=0;',
                                      'finish_real_write();')
            end = harness.rindex('\n}')
            harness = harness[:end] + '\n    cleanup_pages();' + harness[end:]
        if keep_race:
            # Pause a real writer after the keep-path hash read, before its store.
            harness = harness.replace('static uint64_t pkg_hash(',
                                      'static void hash_read_done(void);\nstatic uint64_t pkg_hash(', 1)
            harness = harness.replace('    return h;', '    hash_read_done(); return h;', 1)
            at = harness.index('static void setup_pages(void)')
            hook = r'''
static int write_after_hash;
static void hash_read_done(void) {
    if (!write_after_hash) return;
    write_after_hash=0;write_pending=1;
    assert(!pthread_create(&writer,NULL,cpu_writer,NULL));
    char token;assert(read(ready[0],&token,1)==1 && token=='F');
}
'''
            harness = harness[:at] + hook + harness[at:]
            case = r'''
    if (atoi(argv[1])==6) {
        inject_write=0;pkg_upload_(&im,&t,&f);assert(copies==1);
        im.gpu_w=1;image[0]=77;
        real_dirty_release(memory+8,1); // a neighbouring write releases the shared page
        write_after_hash=1;refresh(&im,&t,&f);
        assert(copies==1 && image[0]==77);finish_write();
        refresh(&im,&t,&f);assert(copies==2 && image[0]==29);
        refresh(&im,&t,&f);assert(copies==2);
        // Adjacent-only changes must still preserve the image written by the GPU.
        image[0]=91;real_dirty_release(memory+8,1);
        refresh(&im,&t,&f);assert(copies==2 && image[0]==91);
        cleanup_pages();return 0;
    }
'''
            harness = harness.replace('    inject_write=1;\n', case + '    inject_write=1;\n', 1)
        with tempfile.TemporaryDirectory(prefix='nx2a-texture-dirty-') as tmp:
            c, exe = Path(tmp) / 'check.c', Path(tmp) / 'check'
            c.write_text(harness)
            subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror',
                            '-Wno-misleading-indentation', '-idirafter', str(headers), str(c), '-o', str(exe)], check=True)
            cases = (('0', 'packaged'), ('1', 'linear')) if real_pages else (('0', 'packaged'), ('1', 'linear'), ('2', 'untracked'))
            if failures:
                cases = (('3', 'allocation failure'), ('4', 'partial mip upload'), ('5', 'invalid format'))
            if keep_race:
                cases = (('6', 'CPU write after GPU-owned texture hash'),)
            for case, label in cases:
                with self.subTest(path=label):
                    subprocess.run([str(exe), case], check=True, timeout=15)
