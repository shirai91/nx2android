// Two-stage replay (DM_VK_MT_REPLAY=1, start-up; off by default): the replay prepares each draw as before (state, descriptor sets, pipelines), but its vkCmd* calls go
// into a queue and a recorder thread of its own makes them, in the same order, into the same command buffers. The driver's share of the replay (Tears of the Kingdom
// at the Great Sky Island pond, 1,900 draws: vkCmdBindDescriptorSets 11.5%, vkCmdDrawIndexed 11.7%, vkCmdBeginRenderPass 8.8%, vkEndCommandBuffer 5.5% of the replay
// thread's samples) then runs beside the rest. Every vkCmd* entry point (gfx_vk.h VK_DEV_FNS) is swapped for a wrapper at vk_init; the command buffers' begin, end and
// allocation, queue submission and presentation first wait until the recorder has made every queued call (vk_dq_drain): command pools are externally synchronized, and a
// submission must hold everything recorded before it. A call whose arguments the wrapper cannot copy (an unknown pNext chain) is made directly after such a wait.
// Producers: the replay thread and the present thread (its blits into its own pool) at once: each record is written under a spinlock
#include "gfx_vk.h"
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DQ_CMDS(X) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdResolveImage) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBeginRendering) \
    X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetDepthBias) X(vkCmdSetLineWidth) X(vkCmdDraw) \
    X(vkCmdPushConstants) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch) X(vkCmdDispatchIndirect) X(vkCmdResetQueryPool) \
    X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdWriteTimestamp) X(vkCmdBindTransformFeedbackBuffersEXT) X(vkCmdBeginTransformFeedbackEXT) X(vkCmdEndTransformFeedbackEXT) X(vkCmdDrawIndirectByteCountEXT) X(vkCmdDrawIndirectCount) X(vkCmdDrawIndexedIndirectCount)
#define DQ_SYNC(X) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkAllocateCommandBuffers) X(vkResetCommandPool) X(vkQueueSubmit) X(vkQueuePresentKHR) X(vkQueueWaitIdle) X(vkDeviceWaitIdle)
#define DQ_REAL(f) static PFN_##f r_##f;
DQ_CMDS(DQ_REAL) DQ_SYNC(DQ_REAL)
#define DQ_OP(f) OP_##f,
enum { OP_WRAP, DQ_CMDS(DQ_OP) OP_N };

// ---- the queue: one producer, one consumer; positions count bytes since the start (the ring index is position % RING), records 16-byte aligned ----
enum { RING = 32 << 20 };
typedef struct { uint32_t op, size; VkCommandBuffer cb; } rec_hdr;   // size: the whole record, header included
// Each side's state on cache lines of its own. Side by side, the recorder's store of its read position after every record took the line that also held the
// producers' write position away from the replay thread's core, and the replay thread paid that miss on every vkCmd* it queued: a Galaxy S25 Ultra's replay thread
// spent 9% of its samples in rec_begin (Breath of the Wild in a village, DM_VK_PROF=1), most of them on the load of the write position right after taking the
// lock. The producers also keep their own copy of the read position (rd_seen) and read the recorder's only when the ring looks full
#define DQ_LINE 128   // (64-byte lines on most Arm cores; 128 also keeps the line next to it apart, which some cores fetch in pairs)
static uint8_t *g_ring;
static struct { _Alignas(DQ_LINE) pthread_spinlock_t lock; uint64_t w, rd_seen; } g_p;   // the producers: lock from rec_begin to rec_end; their write position; the read position last seen
static struct { _Alignas(DQ_LINE) _Atomic uint64_t v; } g_pub_line, g_rd_line;   // published end (the producers write it), the recorder's read position (only it writes it)
static struct { _Alignas(DQ_LINE) _Atomic int sleeping, draining; } g_flags;
#define g_pub g_pub_line.v
#define g_rd g_rd_line.v
#define g_sleeping g_flags.sleeping
#define g_draining g_flags.draining
#define g_pl g_p.lock
#define g_w g_p.w
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER, g_dcv = PTHREAD_COND_INITIALIZER;
static int g_on;
uint64_t g_dq_recs, g_dq_drains, g_dq_direct;   // records queued, drains, calls made directly (unknown arguments)

static void wake(void) { if (atomic_load(&g_sleeping)) { pthread_mutex_lock(&g_mu); pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu); } }
// Waiting for the other side. The two sides used to wait in sched_yield loops: the replay thread at a drain for up to 2,000 calls, the recorder for up to 200
// before it slept; with nothing else to run each call returns at once, so a wait on a fast device kept a core busy in and out of the kernel. On an AYN Odin 3
// (Breath of the Wild at 60 fps, replay 4-6 ms a frame) "nvn replay" and "vk record" used 42-54% of a core each, with 35-65% of the app's CPU time in the
// kernel. Now each side spins on the position itself for up to SPIN_NS without leaving user space, and then sleeps. DM_VK_DQ_SPIN=1 (start-up): the old loops
#define SPIN_NS 50000ull
static int g_spin;
static _Atomic uint64_t g_ns_drain, g_drain_sleeps, g_ns_rec_spin, g_rec_sleeps, g_ns_rec_sleep;   // vk_dq_report
static inline void dq_relax(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}
static uint64_t dq_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static int spin_rd(uint64_t w) {   // until the recorder has read up to w (1), or SPIN_NS passed (0)
    for (uint64_t t0 = 0;;) {
        for (int i = 0; i < 64; i++) { if (atomic_load_explicit(&g_rd, memory_order_acquire) >= w) return 1; dq_relax(); }
        uint64_t t = dq_ns(); if (!t0) t0 = t; else if (t - t0 > SPIN_NS) return 0;
    }
}
static uint64_t spin_pub(uint64_t r) {   // until the published end moves past r (its new value), or SPIN_NS passed (r)
    for (uint64_t t0 = 0;;) {
        for (int i = 0; i < 64; i++) { uint64_t pub = atomic_load(&g_pub); if (pub != r) return pub; dq_relax(); }
        uint64_t t = dq_ns(); if (!t0) t0 = t; else if (t - t0 > SPIN_NS) return r;
    }
}
void vk_dq_drain(void) {
    if (!g_on) return;
    pthread_spin_lock(&g_pl); uint64_t w = g_w; pthread_spin_unlock(&g_pl);   // the records queued so far (another producer may add more meanwhile)
    if (atomic_load_explicit(&g_rd, memory_order_acquire) >= w) return;
    g_dq_drains++; wake();
    uint64_t t0 = dq_ns(); int done;
    if (g_spin) { for (int i = 0; i < 2000 && atomic_load_explicit(&g_rd, memory_order_acquire) < w; i++) sched_yield(); done = atomic_load_explicit(&g_rd, memory_order_acquire) >= w; }   // the recorder is usually a few calls behind
    else done = spin_rd(w);
    if (!done) {
        atomic_fetch_add_explicit(&g_drain_sleeps, 1, memory_order_relaxed);
        pthread_mutex_lock(&g_mu); atomic_fetch_add(&g_draining, 1);   // (a count: the replay and the present thread may both wait)
        int said = 0;
        while (atomic_load_explicit(&g_rd, memory_order_acquire) < w) { pthread_cond_signal(&g_cv);
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 2000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }   // (2 ms: a missed broadcast costs no more)
            if (pthread_cond_timedwait(&g_dcv, &g_mu, &dl) && !said && dq_ns() - t0 > 1000000000ull) { said = 1; uint64_t rd = atomic_load(&g_rd); const rec_hdr *h = (const rec_hdr *)(g_ring + rd % RING); static int n;
                if (n++ < 20) VKLOG("[RED] two-stage replay: a drain waits over 1 s: written %llu, published %llu, read %llu; the record there: op %u size %u",
                    (unsigned long long)w, (unsigned long long)atomic_load(&g_pub), (unsigned long long)rd, h->op, h->size); } }
        atomic_fetch_sub(&g_draining, 1); pthread_mutex_unlock(&g_mu);
    }
    atomic_fetch_add_explicit(&g_ns_drain, dq_ns() - t0, memory_order_relaxed);
}
static void ring_room(uint64_t n) {   // under g_pl: until n more bytes fit; the recorder's read position is read only when the copy says they do not (it only grows)
    while (g_w + n - g_p.rd_seen > RING) {
        g_p.rd_seen = atomic_load_explicit(&g_rd, memory_order_acquire);
        if (g_w + n - g_p.rd_seen <= RING) break;
        wake(); sched_yield();
    }
}
static void *rec_begin(uint32_t op, VkCommandBuffer cb, size_t n) {   // room for a record with n bytes after its header
    size_t need = (sizeof(rec_hdr) + n + 15) & ~(size_t)15;   // 16-byte records: the ring's end always has room for a whole header
    if (need > RING / 4) return NULL;   // (no call copies that much: such a call is made directly)
    pthread_spin_lock(&g_pl);   // released by rec_end
    size_t off = (size_t)(g_w % RING);
    if (off + need > RING) {   // the rest of the ring is skipped
        ring_room(RING - off);
        rec_hdr *h = (rec_hdr *)(g_ring + off); h->op = OP_WRAP; h->size = (uint32_t)(RING - off); g_w += RING - off; atomic_store_explicit(&g_pub, g_w, memory_order_release); off = 0;
    }
    ring_room(need);
    rec_hdr *h = (rec_hdr *)(g_ring + off); h->op = op; h->size = (uint32_t)need; h->cb = cb;
    return h + 1;
}
static void rec_end(void *payload) {
    rec_hdr *h = (rec_hdr *)payload - 1; g_w += h->size; g_dq_recs++;
    atomic_store(&g_pub, g_w);   // (seq_cst, against the recorder's sleep flag: either it sees this record or this sees it asleep)
    pthread_spin_unlock(&g_pl);
    wake();
}
// copying helpers: a record is a fixed struct followed by its arrays
#define ARR(T, p, n) ((n) && (p) ? (size_t)(n) * sizeof(T) : 0)
static uint8_t *put(uint8_t **at, const void *src, size_t n) { uint8_t *d = *at; if (n) memcpy(d, src, n); *at += n; return d; }

// ---- the recorder thread ----
typedef struct { VkPipelineStageFlags src, dst; VkDependencyFlags dep; uint32_t nm, nb, ni; } a_barrier;
typedef struct { VkBuffer s, d; uint32_t n; } a_copybuf;
typedef struct { VkBuffer b; VkImage i; VkImageLayout l; uint32_t n; } a_b2i;
typedef struct { VkImage i; VkImageLayout l; VkBuffer b; uint32_t n; } a_i2b;
typedef struct { VkImage si; VkImageLayout sl; VkImage di; VkImageLayout dl; uint32_t n; VkFilter f; } a_img2;
typedef struct { VkBuffer b; VkDeviceSize off, size; uint32_t data; } a_fill;
typedef struct { VkBuffer b; VkDeviceSize off, size; } a_upd;
typedef struct { VkImage i; VkImageLayout l; VkClearColorValue c; VkClearDepthStencilValue ds; uint32_t n; } a_clrimg;
typedef struct { uint32_t na, nr; } a_clratt;
typedef struct { VkRenderPassBeginInfo bi; VkRenderPassAttachmentBeginInfo ab; int has_ab; VkSubpassContents c; } a_rpbegin;
typedef struct { VkRenderingInfo ri; int has_d, has_s; VkRenderingAttachmentInfo d, s; } a_rbegin;
typedef struct { VkPipelineBindPoint bp; VkPipeline p; } a_bindpipe;
typedef struct { VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, ndyn; } a_sets;
typedef struct { uint32_t first, n; } a_range;
typedef struct { VkBuffer b; VkDeviceSize off; VkIndexType t; } a_index;
typedef struct { float c[4]; } a_blend;
typedef struct { VkStencilFaceFlags f; uint32_t v; } a_stencil;
typedef struct { float c, clamp, slope; } a_bias;
typedef struct { uint32_t a, b, c, d; int32_t e; uint32_t f; } a_draw;
typedef struct { VkPipelineLayout l; VkShaderStageFlags st; uint32_t off, size; } a_push;
typedef struct { VkBuffer b; VkDeviceSize off; uint32_t n, stride; } a_indirect;
typedef struct { VkQueryPool p; uint32_t first, n; } a_qreset;
typedef struct { VkQueryPool p; uint32_t q, flags; } a_query;
typedef struct { VkPipelineStageFlagBits st; VkQueryPool p; uint32_t q; } a_ts;
typedef struct { uint32_t first, n; int has_sizes, has_bufs, has_offs; } a_xfb;
typedef struct { uint32_t inst, first_inst; VkBuffer b; VkDeviceSize off; uint32_t ctr_off, stride; } a_bytecount;
typedef struct { VkBuffer b; VkDeviceSize off; VkBuffer cb; VkDeviceSize coff; uint32_t max, stride; } a_indcount;

static void run(const rec_hdr *h) {
    const uint8_t *p = (const uint8_t *)(h + 1); VkCommandBuffer cb = h->cb;
    switch (h->op) {
    case OP_vkCmdPipelineBarrier: { const a_barrier *a = (const void *)p; const uint8_t *q = p + sizeof *a;
        const VkMemoryBarrier *m = (const void *)q; q += a->nm * sizeof *m; const VkBufferMemoryBarrier *b = (const void *)q; q += a->nb * sizeof *b; const VkImageMemoryBarrier *im = (const void *)q;
        r_vkCmdPipelineBarrier(cb, a->src, a->dst, a->dep, a->nm, a->nm ? m : NULL, a->nb, a->nb ? b : NULL, a->ni, a->ni ? im : NULL); break; }
    case OP_vkCmdCopyBuffer: { const a_copybuf *a = (const void *)p; r_vkCmdCopyBuffer(cb, a->s, a->d, a->n, (const VkBufferCopy *)(p + sizeof *a)); break; }
    case OP_vkCmdCopyBufferToImage: { const a_b2i *a = (const void *)p; r_vkCmdCopyBufferToImage(cb, a->b, a->i, a->l, a->n, (const VkBufferImageCopy *)(p + sizeof *a)); break; }
    case OP_vkCmdCopyImageToBuffer: { const a_i2b *a = (const void *)p; r_vkCmdCopyImageToBuffer(cb, a->i, a->l, a->b, a->n, (const VkBufferImageCopy *)(p + sizeof *a)); break; }
    case OP_vkCmdCopyImage: { const a_img2 *a = (const void *)p; r_vkCmdCopyImage(cb, a->si, a->sl, a->di, a->dl, a->n, (const VkImageCopy *)(p + sizeof *a)); break; }
    case OP_vkCmdBlitImage: { const a_img2 *a = (const void *)p; r_vkCmdBlitImage(cb, a->si, a->sl, a->di, a->dl, a->n, (const VkImageBlit *)(p + sizeof *a), a->f); break; }
    case OP_vkCmdResolveImage: { const a_img2 *a = (const void *)p; r_vkCmdResolveImage(cb, a->si, a->sl, a->di, a->dl, a->n, (const VkImageResolve *)(p + sizeof *a)); break; }
    case OP_vkCmdFillBuffer: { const a_fill *a = (const void *)p; r_vkCmdFillBuffer(cb, a->b, a->off, a->size, a->data); break; }
    case OP_vkCmdUpdateBuffer: { const a_upd *a = (const void *)p; r_vkCmdUpdateBuffer(cb, a->b, a->off, a->size, p + sizeof *a); break; }
    case OP_vkCmdClearColorImage: { const a_clrimg *a = (const void *)p; r_vkCmdClearColorImage(cb, a->i, a->l, &a->c, a->n, (const VkImageSubresourceRange *)(p + sizeof *a)); break; }
    case OP_vkCmdClearDepthStencilImage: { const a_clrimg *a = (const void *)p; r_vkCmdClearDepthStencilImage(cb, a->i, a->l, &a->ds, a->n, (const VkImageSubresourceRange *)(p + sizeof *a)); break; }
    case OP_vkCmdClearAttachments: { const a_clratt *a = (const void *)p; const VkClearAttachment *at = (const void *)(p + sizeof *a);
        r_vkCmdClearAttachments(cb, a->na, at, a->nr, (const VkClearRect *)(at + a->na)); break; }
    case OP_vkCmdBeginRenderPass: { a_rpbegin a; memcpy(&a, p, sizeof a); const uint8_t *q = p + sizeof a;
        a.bi.pClearValues = a.bi.clearValueCount ? (const VkClearValue *)q : NULL; q += a.bi.clearValueCount * sizeof(VkClearValue);
        if (a.has_ab) { a.ab.pAttachments = (const VkImageView *)q; a.bi.pNext = &a.ab; } else a.bi.pNext = NULL;
        r_vkCmdBeginRenderPass(cb, &a.bi, a.c); break; }
    case OP_vkCmdEndRenderPass: r_vkCmdEndRenderPass(cb); break;
    case OP_vkCmdBeginRendering: { a_rbegin a; memcpy(&a, p, sizeof a);
        a.ri.pColorAttachments = a.ri.colorAttachmentCount ? (const VkRenderingAttachmentInfo *)(p + sizeof a) : NULL;
        a.ri.pDepthAttachment = a.has_d ? &a.d : NULL; a.ri.pStencilAttachment = a.has_s ? &a.s : NULL; r_vkCmdBeginRendering(cb, &a.ri); break; }
    case OP_vkCmdEndRendering: r_vkCmdEndRendering(cb); break;
    case OP_vkCmdBindPipeline: { const a_bindpipe *a = (const void *)p; r_vkCmdBindPipeline(cb, a->bp, a->p); break; }
    case OP_vkCmdBindDescriptorSets: { const a_sets *a = (const void *)p; const VkDescriptorSet *s = (const void *)(p + sizeof *a);
        r_vkCmdBindDescriptorSets(cb, a->bp, a->l, a->first, a->n, s, a->ndyn, a->ndyn ? (const uint32_t *)(s + a->n) : NULL); break; }
    case OP_vkCmdBindVertexBuffers: { const a_range *a = (const void *)p; const VkBuffer *b = (const void *)(p + sizeof *a);
        r_vkCmdBindVertexBuffers(cb, a->first, a->n, b, (const VkDeviceSize *)(b + a->n)); break; }
    case OP_vkCmdBindIndexBuffer: { const a_index *a = (const void *)p; r_vkCmdBindIndexBuffer(cb, a->b, a->off, a->t); break; }
    case OP_vkCmdSetViewport: { const a_range *a = (const void *)p; r_vkCmdSetViewport(cb, a->first, a->n, (const VkViewport *)(p + sizeof *a)); break; }
    case OP_vkCmdSetScissor: { const a_range *a = (const void *)p; r_vkCmdSetScissor(cb, a->first, a->n, (const VkRect2D *)(p + sizeof *a)); break; }
    case OP_vkCmdSetBlendConstants: { const a_blend *a = (const void *)p; r_vkCmdSetBlendConstants(cb, a->c); break; }
    case OP_vkCmdSetStencilReference: { const a_stencil *a = (const void *)p; r_vkCmdSetStencilReference(cb, a->f, a->v); break; }
    case OP_vkCmdSetStencilCompareMask: { const a_stencil *a = (const void *)p; r_vkCmdSetStencilCompareMask(cb, a->f, a->v); break; }
    case OP_vkCmdSetStencilWriteMask: { const a_stencil *a = (const void *)p; r_vkCmdSetStencilWriteMask(cb, a->f, a->v); break; }
    case OP_vkCmdSetDepthBias: { const a_bias *a = (const void *)p; r_vkCmdSetDepthBias(cb, a->c, a->clamp, a->slope); break; }
    case OP_vkCmdSetLineWidth: { float w; memcpy(&w, p, sizeof w); r_vkCmdSetLineWidth(cb, w); break; }
    case OP_vkCmdDraw: { const a_draw *a = (const void *)p; r_vkCmdDraw(cb, a->a, a->b, a->c, a->d); break; }
    case OP_vkCmdDrawIndexed: { const a_draw *a = (const void *)p; r_vkCmdDrawIndexed(cb, a->a, a->b, a->c, a->e, a->f); break; }
    case OP_vkCmdPushConstants: { const a_push *a = (const void *)p; r_vkCmdPushConstants(cb, a->l, a->st, a->off, a->size, p + sizeof *a); break; }
    case OP_vkCmdDrawIndirect: { const a_indirect *a = (const void *)p; r_vkCmdDrawIndirect(cb, a->b, a->off, a->n, a->stride); break; }
    case OP_vkCmdDrawIndexedIndirect: { const a_indirect *a = (const void *)p; r_vkCmdDrawIndexedIndirect(cb, a->b, a->off, a->n, a->stride); break; }
    case OP_vkCmdDispatch: { const a_draw *a = (const void *)p; r_vkCmdDispatch(cb, a->a, a->b, a->c); break; }
    case OP_vkCmdDispatchIndirect: { const a_indirect *a = (const void *)p; r_vkCmdDispatchIndirect(cb, a->b, a->off); break; }
    case OP_vkCmdResetQueryPool: { const a_qreset *a = (const void *)p; r_vkCmdResetQueryPool(cb, a->p, a->first, a->n); break; }
    case OP_vkCmdBeginQuery: { const a_query *a = (const void *)p; r_vkCmdBeginQuery(cb, a->p, a->q, a->flags); break; }
    case OP_vkCmdEndQuery: { const a_query *a = (const void *)p; r_vkCmdEndQuery(cb, a->p, a->q); break; }
    case OP_vkCmdWriteTimestamp: { const a_ts *a = (const void *)p; r_vkCmdWriteTimestamp(cb, a->st, a->p, a->q); break; }
    case OP_vkCmdBindTransformFeedbackBuffersEXT: { const a_xfb *a = (const void *)p; const VkBuffer *b = (const void *)(p + sizeof *a); const VkDeviceSize *o = (const void *)(b + a->n);
        r_vkCmdBindTransformFeedbackBuffersEXT(cb, a->first, a->n, b, o, a->has_sizes ? o + a->n : NULL); break; }
    case OP_vkCmdBeginTransformFeedbackEXT: case OP_vkCmdEndTransformFeedbackEXT: { const a_xfb *a = (const void *)p; const VkBuffer *b = (const void *)(p + sizeof *a);
        const VkDeviceSize *o = (const void *)(b + (a->has_bufs ? a->n : 0));
        (h->op == OP_vkCmdBeginTransformFeedbackEXT ? r_vkCmdBeginTransformFeedbackEXT : r_vkCmdEndTransformFeedbackEXT)(cb, a->first, a->n, a->has_bufs ? b : NULL, a->has_offs ? o : NULL); break; }
    case OP_vkCmdDrawIndirectCount: { const a_indcount *a = (const void *)p; r_vkCmdDrawIndirectCount(cb, a->b, a->off, a->cb, a->coff, a->max, a->stride); break; }
    case OP_vkCmdDrawIndexedIndirectCount: { const a_indcount *a = (const void *)p; r_vkCmdDrawIndexedIndirectCount(cb, a->b, a->off, a->cb, a->coff, a->max, a->stride); break; }
    case OP_vkCmdDrawIndirectByteCountEXT: { const a_bytecount *a = (const void *)p; r_vkCmdDrawIndirectByteCountEXT(cb, a->inst, a->first_inst, a->b, a->off, a->ctr_off, a->stride); break; }
    default: break;
    }
}
static void *recorder_main(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk record"); uint64_t r = 0;
    for (;;) {
        uint64_t pub = atomic_load(&g_pub);
        if (r == pub) {
            uint64_t t0 = dq_ns();
            if (g_spin) for (int i = 0; i < 200 && (pub = atomic_load(&g_pub)) == r; i++) sched_yield();   // the producer usually queues the next call within microseconds
            else pub = spin_pub(r);
            uint64_t t1 = dq_ns(); atomic_fetch_add_explicit(&g_ns_rec_spin, t1 - t0, memory_order_relaxed);
            if (pub == r) {
                atomic_fetch_add_explicit(&g_rec_sleeps, 1, memory_order_relaxed);
                pthread_mutex_lock(&g_mu);
                if (atomic_load(&g_draining)) pthread_cond_broadcast(&g_dcv);
                atomic_store(&g_sleeping, 1);
                while ((pub = atomic_load(&g_pub)) == r) pthread_cond_wait(&g_cv, &g_mu);
                atomic_store(&g_sleeping, 0); pthread_mutex_unlock(&g_mu);
                atomic_fetch_add_explicit(&g_ns_rec_sleep, dq_ns() - t1, memory_order_relaxed);
            }
        }
        while (r != pub) { const rec_hdr *h = (const rec_hdr *)(g_ring + r % RING); run(h); r += h->size; atomic_store_explicit(&g_rd, r, memory_order_release); }
        if (atomic_load(&g_draining)) { pthread_mutex_lock(&g_mu); pthread_cond_broadcast(&g_dcv); pthread_mutex_unlock(&g_mu); }
    }
    return NULL;
}

// ---- the wrappers (the producer) ----
#define DIRECT(f, ...) do { vk_dq_drain(); g_dq_direct++; r_##f(__VA_ARGS__); } while (0)
#define BEGIN(op, T, extra) uint8_t *base_ = rec_begin(op, cb, sizeof(T) + (extra)); if (!base_) goto direct; T *a = (T *)base_; uint8_t *at_ = base_ + sizeof(T)
static VKAPI_ATTR void VKAPI_CALL w_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier *m,
                                                         uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    for (uint32_t i = 0; i < nm; i++) if (m[i].pNext) goto direct;
    for (uint32_t i = 0; i < nb; i++) if (b[i].pNext) goto direct;
    for (uint32_t i = 0; i < ni; i++) if (im[i].pNext) goto direct;
    { BEGIN(OP_vkCmdPipelineBarrier, a_barrier, ARR(VkMemoryBarrier, m, nm) + ARR(VkBufferMemoryBarrier, b, nb) + ARR(VkImageMemoryBarrier, im, ni));
      *a = (a_barrier){src, dst, dep, m ? nm : 0, b ? nb : 0, im ? ni : 0}; put(&at_, m, ARR(VkMemoryBarrier, m, nm)); put(&at_, b, ARR(VkBufferMemoryBarrier, b, nb)); put(&at_, im, ARR(VkImageMemoryBarrier, im, ni));
      rec_end(base_); return; }
direct: DIRECT(vkCmdPipelineBarrier, cb, src, dst, dep, nm, m, nb, b, ni, im);
}
#define W_REGIONS(f, OP, T, R, sig, init, call) \
    static VKAPI_ATTR void VKAPI_CALL w_##f sig { { BEGIN(OP, T, ARR(R, rg, n)); *a = init; put(&at_, rg, ARR(R, rg, n)); rec_end(base_); return; } direct: DIRECT call; }
W_REGIONS(vkCmdCopyBuffer, OP_vkCmdCopyBuffer, a_copybuf, VkBufferCopy, (VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *rg), ((a_copybuf){s, d, n}), (vkCmdCopyBuffer, cb, s, d, n, rg))
W_REGIONS(vkCmdCopyBufferToImage, OP_vkCmdCopyBufferToImage, a_b2i, VkBufferImageCopy, (VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy *rg),
          ((a_b2i){b, i, l, n}), (vkCmdCopyBufferToImage, cb, b, i, l, n, rg))
W_REGIONS(vkCmdCopyImageToBuffer, OP_vkCmdCopyImageToBuffer, a_i2b, VkBufferImageCopy, (VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *rg),
          ((a_i2b){i, l, b, n}), (vkCmdCopyImageToBuffer, cb, i, l, b, n, rg))
W_REGIONS(vkCmdCopyImage, OP_vkCmdCopyImage, a_img2, VkImageCopy, (VkCommandBuffer cb, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl, uint32_t n, const VkImageCopy *rg),
          ((a_img2){si, sl, di, dl, n, VK_FILTER_NEAREST}), (vkCmdCopyImage, cb, si, sl, di, dl, n, rg))
W_REGIONS(vkCmdBlitImage, OP_vkCmdBlitImage, a_img2, VkImageBlit, (VkCommandBuffer cb, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl, uint32_t n, const VkImageBlit *rg, VkFilter f),
          ((a_img2){si, sl, di, dl, n, f}), (vkCmdBlitImage, cb, si, sl, di, dl, n, rg, f))
W_REGIONS(vkCmdResolveImage, OP_vkCmdResolveImage, a_img2, VkImageResolve, (VkCommandBuffer cb, VkImage si, VkImageLayout sl, VkImage di, VkImageLayout dl, uint32_t n, const VkImageResolve *rg),
          ((a_img2){si, sl, di, dl, n, VK_FILTER_NEAREST}), (vkCmdResolveImage, cb, si, sl, di, dl, n, rg))
W_REGIONS(vkCmdSetViewport, OP_vkCmdSetViewport, a_range, VkViewport, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *rg), ((a_range){first, n}), (vkCmdSetViewport, cb, first, n, rg))
W_REGIONS(vkCmdSetScissor, OP_vkCmdSetScissor, a_range, VkRect2D, (VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *rg), ((a_range){first, n}), (vkCmdSetScissor, cb, first, n, rg))
static VKAPI_ATTR void VKAPI_CALL w_vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, uint32_t data) {
    { BEGIN(OP_vkCmdFillBuffer, a_fill, 0); *a = (a_fill){b, off, size, data}; (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdFillBuffer, cb, b, off, size, data); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, const void *data) {
    { BEGIN(OP_vkCmdUpdateBuffer, a_upd, (size_t)size); *a = (a_upd){b, off, size}; put(&at_, data, (size_t)size); rec_end(base_); return; } direct: DIRECT(vkCmdUpdateBuffer, cb, b, off, size, data); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *rg) {
    { BEGIN(OP_vkCmdClearColorImage, a_clrimg, ARR(VkImageSubresourceRange, rg, n)); memset(a, 0, sizeof *a); a->i = i; a->l = l; a->c = *c; a->n = n; put(&at_, rg, ARR(VkImageSubresourceRange, rg, n)); rec_end(base_); return; }
    direct: DIRECT(vkCmdClearColorImage, cb, i, l, c, n, rg); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *ds, uint32_t n, const VkImageSubresourceRange *rg) {
    { BEGIN(OP_vkCmdClearDepthStencilImage, a_clrimg, ARR(VkImageSubresourceRange, rg, n)); memset(a, 0, sizeof *a); a->i = i; a->l = l; a->ds = *ds; a->n = n; put(&at_, rg, ARR(VkImageSubresourceRange, rg, n)); rec_end(base_); return; }
    direct: DIRECT(vkCmdClearDepthStencilImage, cb, i, l, ds, n, rg); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *at, uint32_t nr, const VkClearRect *rc) {
    { BEGIN(OP_vkCmdClearAttachments, a_clratt, ARR(VkClearAttachment, at, na) + ARR(VkClearRect, rc, nr)); *a = (a_clratt){na, nr};
      put(&at_, at, ARR(VkClearAttachment, at, na)); put(&at_, rc, ARR(VkClearRect, rc, nr)); rec_end(base_); return; }
    direct: DIRECT(vkCmdClearAttachments, cb, na, at, nr, rc); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c) {
    const VkRenderPassAttachmentBeginInfo *ab = NULL;
    if (bi->pNext) { const VkBaseInStructure *x = bi->pNext; if (x->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO || x->pNext) goto direct; ab = (const void *)x; }
    { size_t nc = ARR(VkClearValue, bi->pClearValues, bi->clearValueCount), nv = ab ? ARR(VkImageView, ab->pAttachments, ab->attachmentCount) : 0;
      BEGIN(OP_vkCmdBeginRenderPass, a_rpbegin, nc + nv); memset(a, 0, sizeof *a); a->bi = *bi; a->c = c;
      if (!bi->pClearValues) a->bi.clearValueCount = 0;
      if (ab) { a->ab = *ab; a->has_ab = 1; }
      put(&at_, bi->pClearValues, nc); put(&at_, ab ? ab->pAttachments : NULL, nv); rec_end(base_); return; }
direct: DIRECT(vkCmdBeginRenderPass, cb, bi, c);
}
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    if (ri->pNext || (ri->pDepthAttachment && ri->pDepthAttachment->pNext) || (ri->pStencilAttachment && ri->pStencilAttachment->pNext)) goto direct;
    for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) if (ri->pColorAttachments[i].pNext) goto direct;
    { BEGIN(OP_vkCmdBeginRendering, a_rbegin, ARR(VkRenderingAttachmentInfo, ri->pColorAttachments, ri->colorAttachmentCount)); memset(a, 0, sizeof *a); a->ri = *ri;
      if (!ri->pColorAttachments) a->ri.colorAttachmentCount = 0;
      if (ri->pDepthAttachment) { a->d = *ri->pDepthAttachment; a->has_d = 1; }
      if (ri->pStencilAttachment) { a->s = *ri->pStencilAttachment; a->has_s = 1; }
      put(&at_, ri->pColorAttachments, ARR(VkRenderingAttachmentInfo, ri->pColorAttachments, ri->colorAttachmentCount)); rec_end(base_); return; }
direct: DIRECT(vkCmdBeginRendering, cb, ri);
}
#define W_NOARG(f) static VKAPI_ATTR void VKAPI_CALL w_##f(VkCommandBuffer cb) { void *a = rec_begin(OP_##f, cb, 0); if (!a) { DIRECT(f, cb); return; } rec_end(a); }
W_NOARG(vkCmdEndRenderPass) W_NOARG(vkCmdEndRendering)
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pl) {
    { BEGIN(OP_vkCmdBindPipeline, a_bindpipe, 0); *a = (a_bindpipe){bp, pl}; (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdBindPipeline, cb, bp, pl); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) {
    { BEGIN(OP_vkCmdBindDescriptorSets, a_sets, ARR(VkDescriptorSet, s, n) + ARR(uint32_t, d, nd)); *a = (a_sets){bp, l, first, n, d ? nd : 0};
      put(&at_, s, ARR(VkDescriptorSet, s, n)); put(&at_, d, ARR(uint32_t, d, nd)); rec_end(base_); return; }
    direct: DIRECT(vkCmdBindDescriptorSets, cb, bp, l, first, n, s, nd, d); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    { BEGIN(OP_vkCmdBindVertexBuffers, a_range, ARR(VkBuffer, b, n) + ARR(VkDeviceSize, o, n)); *a = (a_range){first, n}; put(&at_, b, ARR(VkBuffer, b, n)); put(&at_, o, ARR(VkDeviceSize, o, n)); rec_end(base_); return; }
    direct: DIRECT(vkCmdBindVertexBuffers, cb, first, n, b, o); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkIndexType t) {
    { BEGIN(OP_vkCmdBindIndexBuffer, a_index, 0); *a = (a_index){b, off, t}; (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdBindIndexBuffer, cb, b, off, t); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    { BEGIN(OP_vkCmdSetBlendConstants, a_blend, 0); memcpy(a->c, c, sizeof a->c); (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdSetBlendConstants, cb, c); }
#define W_STENCIL(f) static VKAPI_ATTR void VKAPI_CALL w_##f(VkCommandBuffer cb, VkStencilFaceFlags fl, uint32_t v) { \
    { BEGIN(OP_##f, a_stencil, 0); *a = (a_stencil){fl, v}; (void)at_; rec_end(base_); return; } direct: DIRECT(f, cb, fl, v); }
W_STENCIL(vkCmdSetStencilReference) W_STENCIL(vkCmdSetStencilCompareMask) W_STENCIL(vkCmdSetStencilWriteMask)
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetDepthBias(VkCommandBuffer cb, float c, float clamp, float slope) {
    { BEGIN(OP_vkCmdSetDepthBias, a_bias, 0); *a = (a_bias){c, clamp, slope}; (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdSetDepthBias, cb, c, clamp, slope); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetLineWidth(VkCommandBuffer cb, float w) {
    { BEGIN(OP_vkCmdSetLineWidth, float, 0); *a = w; (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdSetLineWidth, cb, w); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    { BEGIN(OP_vkCmdDraw, a_draw, 0); *a = (a_draw){vc, ic, fv, fi, 0, 0}; (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdDraw, cb, vc, ic, fv, fi); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t n, uint32_t fi, int32_t vo, uint32_t fin) {
    { BEGIN(OP_vkCmdDrawIndexed, a_draw, 0); *a = (a_draw){ic, n, fi, 0, vo, fin}; (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdDrawIndexed, cb, ic, n, fi, vo, fin); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *v) {
    { BEGIN(OP_vkCmdPushConstants, a_push, size); *a = (a_push){l, st, off, size}; put(&at_, v, size); rec_end(base_); return; } direct: DIRECT(vkCmdPushConstants, cb, l, st, off, size, v); }
#define W_INDIRECT(f) static VKAPI_ATTR void VKAPI_CALL w_##f(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, uint32_t n, uint32_t stride) { \
    { BEGIN(OP_##f, a_indirect, 0); *a = (a_indirect){b, off, n, stride}; (void)at_; rec_end(base_); return; } direct: DIRECT(f, cb, b, off, n, stride); }
W_INDIRECT(vkCmdDrawIndirect) W_INDIRECT(vkCmdDrawIndexedIndirect)
#define W_INDCOUNT(f) static VKAPI_ATTR void VKAPI_CALL w_##f(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkBuffer c, VkDeviceSize coff, uint32_t max, uint32_t stride) { \
    { BEGIN(OP_##f, a_indcount, 0); *a = (a_indcount){b, off, c, coff, max, stride}; (void)at_; rec_end(base_); return; } direct: DIRECT(f, cb, b, off, c, coff, max, stride); }
W_INDCOUNT(vkCmdDrawIndirectCount) W_INDCOUNT(vkCmdDrawIndexedIndirectCount)
static VKAPI_ATTR void VKAPI_CALL w_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    { BEGIN(OP_vkCmdDispatch, a_draw, 0); *a = (a_draw){x, y, z, 0, 0, 0}; (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdDispatch, cb, x, y, z); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off) {
    { BEGIN(OP_vkCmdDispatchIndirect, a_indirect, 0); *a = (a_indirect){b, off, 0, 0}; (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdDispatchIndirect, cb, b, off); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool p, uint32_t first, uint32_t n) {
    { BEGIN(OP_vkCmdResetQueryPool, a_qreset, 0); *a = (a_qreset){p, first, n}; (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdResetQueryPool, cb, p, first, n); }
// the occlusion queries of gfx_vk_occlusion.inc: without these in the queue they would be recorded on the replay thread while the recorder is
// making its own calls into the same command buffer (two threads, one command buffer: undefined, and Turnip faults or loses the device)
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBeginQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f) {
    { BEGIN(OP_vkCmdBeginQuery, a_query, 0); *a = (a_query){p, q, f}; (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdBeginQuery, cb, p, q, f); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdEndQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q) {
    { BEGIN(OP_vkCmdEndQuery, a_query, 0); *a = (a_query){p, q, 0}; (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdEndQuery, cb, p, q); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool p, uint32_t q) {
    { BEGIN(OP_vkCmdWriteTimestamp, a_ts, 0); *a = (a_ts){st, p, q}; (void)at_; rec_end(base_); return; } direct: DIRECT(vkCmdWriteTimestamp, cb, st, p, q); }
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBindTransformFeedbackBuffersEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o, const VkDeviceSize *sz) {
    { BEGIN(OP_vkCmdBindTransformFeedbackBuffersEXT, a_xfb, ARR(VkBuffer, b, n) + ARR(VkDeviceSize, o, n) + ARR(VkDeviceSize, sz, n)); *a = (a_xfb){first, n, sz != NULL, 1, 1};
      put(&at_, b, ARR(VkBuffer, b, n)); put(&at_, o, ARR(VkDeviceSize, o, n)); put(&at_, sz, ARR(VkDeviceSize, sz, n)); rec_end(base_); return; }
    direct: DIRECT(vkCmdBindTransformFeedbackBuffersEXT, cb, first, n, b, o, sz); }
#define W_XFB(f) static VKAPI_ATTR void VKAPI_CALL w_##f(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) { \
    { BEGIN(OP_##f, a_xfb, ARR(VkBuffer, b, n) + ARR(VkDeviceSize, o, n)); *a = (a_xfb){first, n, 0, b != NULL, o != NULL}; \
      put(&at_, b, ARR(VkBuffer, b, n)); put(&at_, o, ARR(VkDeviceSize, o, n)); rec_end(base_); return; } direct: DIRECT(f, cb, first, n, b, o); }
W_XFB(vkCmdBeginTransformFeedbackEXT) W_XFB(vkCmdEndTransformFeedbackEXT)
static VKAPI_ATTR void VKAPI_CALL w_vkCmdDrawIndirectByteCountEXT(VkCommandBuffer cb, uint32_t inst, uint32_t first_inst, VkBuffer b, VkDeviceSize off, uint32_t ctr_off, uint32_t stride) {
    { BEGIN(OP_vkCmdDrawIndirectByteCountEXT, a_bytecount, 0); *a = (a_bytecount){inst, first_inst, b, off, ctr_off, stride}; (void)at_; rec_end(base_); return; }
    direct: DIRECT(vkCmdDrawIndirectByteCountEXT, cb, inst, first_inst, b, off, ctr_off, stride); }

// the calls that own command pools or the queue: after every queued call is made
static VKAPI_ATTR VkResult VKAPI_CALL w_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) { vk_dq_drain(); return r_vkBeginCommandBuffer(cb, bi); }
static VKAPI_ATTR VkResult VKAPI_CALL w_vkEndCommandBuffer(VkCommandBuffer cb) { vk_dq_drain(); return r_vkEndCommandBuffer(cb); }
static VKAPI_ATTR VkResult VKAPI_CALL w_vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *cb) { vk_dq_drain(); return r_vkAllocateCommandBuffers(d, ai, cb); }
static VKAPI_ATTR VkResult VKAPI_CALL w_vkResetCommandPool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) { vk_dq_drain(); return r_vkResetCommandPool(d, p, f); }
static VKAPI_ATTR VkResult VKAPI_CALL w_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) { vk_dq_drain(); return r_vkQueueSubmit(q, n, si, f); }
static VKAPI_ATTR VkResult VKAPI_CALL w_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) { vk_dq_drain(); return r_vkQueuePresentKHR(q, pi); }
static VKAPI_ATTR VkResult VKAPI_CALL w_vkQueueWaitIdle(VkQueue q) { vk_dq_drain(); return r_vkQueueWaitIdle(q); }
static VKAPI_ATTR VkResult VKAPI_CALL w_vkDeviceWaitIdle(VkDevice d) { vk_dq_drain(); return r_vkDeviceWaitIdle(d); }

void vk_dq_init(void) {   // vk_init, after the device's entry points are loaded
    const char *e = getenv("DM_VK_MT_REPLAY"); if (!(e && atoi(e) > 0)) return;
    pthread_spin_init(&g_pl, PTHREAD_PROCESS_PRIVATE);
    if (!(g_ring = malloc(RING))) { VKLOG("[RED] DM_VK_MT_REPLAY: no memory for the queue; recording on the replay thread"); return; }
#define DQ_SWAP(f) r_##f = f; if (r_##f) f = w_##f;
    DQ_CMDS(DQ_SWAP) DQ_SYNC(DQ_SWAP)
    { const char *sp = getenv("DM_VK_DQ_SPIN"); g_spin = sp && atoi(sp) > 0; if (g_spin) VKLOG("DM_VK_DQ_SPIN=1: the replay queue waits in sched_yield loops (as before)"); }
    g_on = 1;
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, recorder_main, NULL)) die("[vk] recorder thread");
    pthread_attr_destroy(&at);
    VKLOG("two-stage replay: vkCmd* calls queued for the recorder thread (DM_VK_MT_REPLAY)");
}

// Every 600 presents (gfx_vk_present.c, beside "replay per frame"): the queue's records and drains, the producers' time waiting at drains, and the recorder's time
// spinning idle and asleep (the rest of the period it was making calls)
void vk_dq_report(void) {
    if (!g_on) return;
    static uint64_t pr, pd, pns, pds, psp, psl, pnsl, pt;
    uint64_t now = dq_ns(), recs = g_dq_recs, drains = g_dq_drains, ns = atomic_load(&g_ns_drain), ds = atomic_load(&g_drain_sleeps), sp = atomic_load(&g_ns_rec_spin),
             sl = atomic_load(&g_rec_sleeps), nsl = atomic_load(&g_ns_rec_sleep);
    if (pt) { double wall = (double)(now - pt), busy = wall - (double)(sp - psp) - (double)(nsl - pnsl);
        VKLOG("replay queue per frame (last 600): %.0f records, %.1f drains (%.1f slept), waiting at drains %.2f ms; recorder making calls %.2f ms, spinning %.2f ms, asleep %.2f ms (%.1f times)%s",
              (recs - pr) / 600.0, (drains - pd) / 600.0, (ds - pds) / 600.0, (ns - pns) / 6e8, (busy > 0 ? busy : 0) / 6e8, (sp - psp) / 6e8, (nsl - pnsl) / 6e8, (sl - psl) / 600.0,
              g_spin ? " (DM_VK_DQ_SPIN)" : ""); }
    pr = recs; pd = drains; pns = ns; pds = ds; psp = sp; psl = sl; pnsl = nsl; pt = now;
}
