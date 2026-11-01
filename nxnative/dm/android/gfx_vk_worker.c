// The replay's Vulkan command recording on a thread of its own, as Eden's Scheduler does it (its worker thread runs the command chunks the GPU thread queues): with
// DM_VK_WORKER=1 (start-up) the vkCmd* functions and vkBeginCommandBuffer, for the submission slots' command buffers, only copy their arguments into a queue, and the
// worker thread makes the driver calls in the same order. FINAL FANTASY RESONANCE's heavy scene spent about 40% of the replay thread inside Adreno's driver (its
// vkCmd* calls and the libc work they do; POCO F7, 2026-11-01), which now runs beside the replay's own work.
// vkEndCommandBuffer and vkQueueSubmit wait until the queue is empty and then run on the calling thread, so a refused command buffer is seen where it was, and a
// submission holds everything recorded before it. Descriptor sets are still allocated and written on the replay thread: a set is written once, before the bind that
// uses it is queued, and its pool is reset only after the GPU has passed the submission (slot_recycle). Command buffers that are not a slot's (the present's, the
// overlay's) are recorded on the spot.
#include "gfx_vk.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>

enum {
    W_BARRIER, W_COPY_BUF, W_COPY_B2I, W_COPY_I2B, W_COPY_IMG, W_BLIT, W_FILL, W_UPDATE, W_CLEAR_COLOR, W_CLEAR_DS, W_CLEAR_ATT, W_RESOLVE,
    W_BEGIN_RP, W_END_RP, W_BEGIN_R, W_END_R, W_BIND_PIPE, W_BIND_SETS, W_BIND_VB, W_BIND_IB, W_VIEWPORT, W_SCISSOR, W_BLEND, W_ST_REF, W_ST_CMP, W_ST_WR,
    W_DEPTH_BIAS, W_LINE_W, W_DRAW, W_PUSH, W_DRAW_IDX, W_DRAW_IND, W_DRAW_IDX_IND, W_DISPATCH, W_DISPATCH_IND, W_RESET_QP, W_TIMESTAMP, W_XFB_BIND, W_XFB_BEGIN,
    W_XFB_END, W_BEGIN_CB,
};

// The queue: blocks of bytes, filled by the one thread recording a submission (they take turns under g_sub_lock) and run by the worker in order. A record is reserved
// whole before it is written (w_reserve), so it never spans two blocks; a block whose writer has moved on is closed: all of it is published
enum { WBLK = 1 << 20 };
typedef struct wblk { struct wblk *next; size_t used; int closed; } wblk;   // used: bytes published to the worker (under g_wq_mu)
static wblk *g_wq_head, *g_wq_tail, *g_wq_free; static pthread_mutex_t g_wq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_wq_cv = PTHREAD_COND_INITIALIZER, g_wq_idle = PTHREAD_COND_INITIALIZER;
static int g_wq_on, g_wq_busy;   // busy: the worker is running records it took
static uint64_t g_wq_pub, g_wq_ran;   // records published, records run (under g_wq_mu)
static uint64_t g_wq_pending;   // records written but not yet published
static uint8_t *w_at, *w_base; static size_t w_left;   // the writer's place in the tail block (one writer at a time)
static uint8_t *blk_data(wblk *b) { return (uint8_t *)(b + 1); }
static void w_reserve(size_t n) {   // room for a whole record in the tail block
    if (n <= w_left) return;
    if (n > WBLK) die("[vk] worker queue: a record of %zu bytes", n);
    pthread_mutex_lock(&g_wq_mu);
    if (g_wq_tail) { g_wq_tail->used = (size_t)(w_at - w_base); g_wq_tail->closed = 1; g_wq_pub += g_wq_pending; g_wq_pending = 0; }
    wblk *b = g_wq_free; if (b) g_wq_free = b->next; else if (!(b = malloc(sizeof *b + WBLK))) die("[vk] worker queue: out of memory");
    b->next = NULL; b->used = 0; b->closed = 0;
    if (g_wq_tail) g_wq_tail->next = b; else g_wq_head = b;
    g_wq_tail = b; pthread_cond_signal(&g_wq_cv); pthread_mutex_unlock(&g_wq_mu);
    w_base = w_at = blk_data(b); w_left = WBLK;
}
static void *w_room(size_t n) { n = (n + 7) & ~(size_t)7; if (n > w_left) die("[vk] worker queue: a record outgrew its reservation"); void *p = w_at; w_at += n; w_left -= n; return p; }
#define W_PUT(T, v) (*(T *)w_room(sizeof(T)) = (v))
static void *w_copy(const void *src, size_t n) { void *p = w_room(n); if (n) memcpy(p, src, n); return p; }
#define WA(n) ((((size_t)(n)) + 7) & ~(size_t)7)   // an array's room in a record
static void w_publish(void) {   // what has been written goes to the worker
    if (!g_wq_tail) return;
    pthread_mutex_lock(&g_wq_mu); g_wq_tail->used = (size_t)(w_at - w_base); g_wq_pub += g_wq_pending; g_wq_pending = 0;
    pthread_cond_signal(&g_wq_cv); pthread_mutex_unlock(&g_wq_mu);
}
static void w_end(void) { if (++g_wq_pending >= 32) w_publish(); }   // a record written: published in groups, the worker runs them while the next are written

// The command buffers that go through the queue: the submission slots' (vk_worker_track)
enum { WCB_N = 4096 };
static VkCommandBuffer g_wcb[WCB_N]; static int g_nwcb; static pthread_mutex_t g_wcb_mu = PTHREAD_MUTEX_INITIALIZER;
void vk_worker_track(VkCommandBuffer cb) { if (!g_wq_on) return; pthread_mutex_lock(&g_wcb_mu); if (g_nwcb < WCB_N) g_wcb[g_nwcb++] = cb; pthread_mutex_unlock(&g_wcb_mu); }
static int queued(VkCommandBuffer cb) {   // (a slot's command buffers are made at start-up and as submissions grow, never freed)
    static __thread VkCommandBuffer last; if (cb == last) return 1;
    int n = __atomic_load_n(&g_nwcb, __ATOMIC_ACQUIRE); for (int i = 0; i < n; i++) if (g_wcb[i] == cb) { last = cb; return 1; }
    return 0;
}

// The driver's own functions, kept when the recorders take their place
#define W_REAL(f) static PFN_##f r_##f;
#define W_FNS(X) X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdFillBuffer) \
    X(vkCmdUpdateBuffer) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdResolveImage) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdSetViewport) \
    X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetDepthBias) X(vkCmdSetLineWidth) \
    X(vkCmdDraw) X(vkCmdPushConstants) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch) X(vkCmdDispatchIndirect) X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp) X(vkCmdBindTransformFeedbackBuffersEXT) X(vkCmdBeginTransformFeedbackEXT) X(vkCmdEndTransformFeedbackEXT) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkQueueSubmit)
W_FNS(W_REAL)

// Reading one back on the worker
typedef struct { const uint8_t *p; } wrd;
static const void *r_take(wrd *r, size_t n) { const void *p = r->p; r->p += (n + 7) & ~(size_t)7; return p; }
#define R_GET(T) (*(const T *)r_take(&rd, sizeof(T)))

static void run_one(wrd *r_) {
    wrd rd = *r_; int op = (int)R_GET(uint64_t); VkCommandBuffer cb = R_GET(VkCommandBuffer);
    switch (op) {
    case W_BARRIER: { VkPipelineStageFlags s = R_GET(uint32_t), d = R_GET(uint32_t); VkDependencyFlags f = R_GET(uint32_t); uint32_t nm = R_GET(uint32_t), nb = R_GET(uint32_t), ni = R_GET(uint32_t);
        const VkMemoryBarrier *m = r_take(&rd, sizeof *m * nm); const VkBufferMemoryBarrier *b = r_take(&rd, sizeof *b * nb); const VkImageMemoryBarrier *im = r_take(&rd, sizeof *im * ni);
        r_vkCmdPipelineBarrier(cb, s, d, f, nm, nm ? m : NULL, nb, nb ? b : NULL, ni, ni ? im : NULL); break; }
    case W_COPY_BUF: { VkBuffer s = R_GET(VkBuffer), d = R_GET(VkBuffer); uint32_t n = R_GET(uint32_t); r_vkCmdCopyBuffer(cb, s, d, n, r_take(&rd, sizeof(VkBufferCopy) * n)); break; }
    case W_COPY_B2I: { VkBuffer s = R_GET(VkBuffer); VkImage d = R_GET(VkImage); VkImageLayout l = R_GET(uint32_t); uint32_t n = R_GET(uint32_t); r_vkCmdCopyBufferToImage(cb, s, d, l, n, r_take(&rd, sizeof(VkBufferImageCopy) * n)); break; }
    case W_COPY_I2B: { VkImage s = R_GET(VkImage); VkImageLayout l = R_GET(uint32_t); VkBuffer d = R_GET(VkBuffer); uint32_t n = R_GET(uint32_t); r_vkCmdCopyImageToBuffer(cb, s, l, d, n, r_take(&rd, sizeof(VkBufferImageCopy) * n)); break; }
    case W_COPY_IMG: { VkImage s = R_GET(VkImage); VkImageLayout sl = R_GET(uint32_t); VkImage d = R_GET(VkImage); VkImageLayout dl = R_GET(uint32_t); uint32_t n = R_GET(uint32_t);
        r_vkCmdCopyImage(cb, s, sl, d, dl, n, r_take(&rd, sizeof(VkImageCopy) * n)); break; }
    case W_BLIT: { VkImage s = R_GET(VkImage); VkImageLayout sl = R_GET(uint32_t); VkImage d = R_GET(VkImage); VkImageLayout dl = R_GET(uint32_t); uint32_t n = R_GET(uint32_t); VkFilter f = R_GET(uint32_t);
        r_vkCmdBlitImage(cb, s, sl, d, dl, n, r_take(&rd, sizeof(VkImageBlit) * n), f); break; }
    case W_FILL: { VkBuffer b = R_GET(VkBuffer); VkDeviceSize o = R_GET(VkDeviceSize), n = R_GET(VkDeviceSize); uint32_t v = R_GET(uint32_t); r_vkCmdFillBuffer(cb, b, o, n, v); break; }
    case W_UPDATE: { VkBuffer b = R_GET(VkBuffer); VkDeviceSize o = R_GET(VkDeviceSize), n = R_GET(VkDeviceSize); r_vkCmdUpdateBuffer(cb, b, o, n, r_take(&rd, (size_t)n)); break; }
    case W_CLEAR_COLOR: { VkImage im = R_GET(VkImage); VkImageLayout l = R_GET(uint32_t); VkClearColorValue c = R_GET(VkClearColorValue); uint32_t n = R_GET(uint32_t);
        r_vkCmdClearColorImage(cb, im, l, &c, n, r_take(&rd, sizeof(VkImageSubresourceRange) * n)); break; }
    case W_CLEAR_DS: { VkImage im = R_GET(VkImage); VkImageLayout l = R_GET(uint32_t); VkClearDepthStencilValue c = R_GET(VkClearDepthStencilValue); uint32_t n = R_GET(uint32_t);
        r_vkCmdClearDepthStencilImage(cb, im, l, &c, n, r_take(&rd, sizeof(VkImageSubresourceRange) * n)); break; }
    case W_CLEAR_ATT: { uint32_t na = R_GET(uint32_t), nr = R_GET(uint32_t); const VkClearAttachment *a = r_take(&rd, sizeof *a * na); r_vkCmdClearAttachments(cb, na, a, nr, r_take(&rd, sizeof(VkClearRect) * nr)); break; }
    case W_RESOLVE: { VkImage s = R_GET(VkImage); VkImageLayout sl = R_GET(uint32_t); VkImage d = R_GET(VkImage); VkImageLayout dl = R_GET(uint32_t); uint32_t n = R_GET(uint32_t);
        r_vkCmdResolveImage(cb, s, sl, d, dl, n, r_take(&rd, sizeof(VkImageResolve) * n)); break; }
    case W_BEGIN_RP: { VkRenderPassBeginInfo bi = R_GET(VkRenderPassBeginInfo); VkSubpassContents c = R_GET(uint32_t); int has_abi = (int)R_GET(uint32_t); VkRenderPassAttachmentBeginInfo abi;
        bi.pClearValues = bi.clearValueCount ? r_take(&rd, sizeof(VkClearValue) * bi.clearValueCount) : NULL; bi.pNext = NULL;
        if (has_abi) { abi = R_GET(VkRenderPassAttachmentBeginInfo); abi.pNext = NULL; abi.pAttachments = r_take(&rd, sizeof(VkImageView) * abi.attachmentCount); bi.pNext = &abi; }
        r_vkCmdBeginRenderPass(cb, &bi, c); break; }
    case W_END_RP: r_vkCmdEndRenderPass(cb); break;
    case W_BEGIN_R: { VkRenderingInfo ri = R_GET(VkRenderingInfo); int hd = (int)R_GET(uint32_t), hs = (int)R_GET(uint32_t);
        ri.pNext = NULL; ri.pColorAttachments = ri.colorAttachmentCount ? r_take(&rd, sizeof(VkRenderingAttachmentInfo) * ri.colorAttachmentCount) : NULL;
        ri.pDepthAttachment = hd ? r_take(&rd, sizeof(VkRenderingAttachmentInfo)) : NULL; ri.pStencilAttachment = hs ? r_take(&rd, sizeof(VkRenderingAttachmentInfo)) : NULL;
        r_vkCmdBeginRendering(cb, &ri); break; }
    case W_END_R: r_vkCmdEndRendering(cb); break;
    case W_BIND_PIPE: { VkPipelineBindPoint bp = R_GET(uint32_t); VkPipeline p = R_GET(VkPipeline); r_vkCmdBindPipeline(cb, bp, p); break; }
    case W_BIND_SETS: { VkPipelineBindPoint bp = R_GET(uint32_t); VkPipelineLayout l = R_GET(VkPipelineLayout); uint32_t first = R_GET(uint32_t), n = R_GET(uint32_t), nd = R_GET(uint32_t);
        const VkDescriptorSet *s = r_take(&rd, sizeof *s * n); r_vkCmdBindDescriptorSets(cb, bp, l, first, n, s, nd, nd ? r_take(&rd, 4 * (size_t)nd) : NULL); break; }
    case W_BIND_VB: { uint32_t first = R_GET(uint32_t), n = R_GET(uint32_t); const VkBuffer *b = r_take(&rd, sizeof *b * n); r_vkCmdBindVertexBuffers(cb, first, n, b, r_take(&rd, sizeof(VkDeviceSize) * n)); break; }
    case W_BIND_IB: { VkBuffer b = R_GET(VkBuffer); VkDeviceSize o = R_GET(VkDeviceSize); VkIndexType t = R_GET(uint32_t); r_vkCmdBindIndexBuffer(cb, b, o, t); break; }
    case W_VIEWPORT: { uint32_t f = R_GET(uint32_t), n = R_GET(uint32_t); r_vkCmdSetViewport(cb, f, n, r_take(&rd, sizeof(VkViewport) * n)); break; }
    case W_SCISSOR: { uint32_t f = R_GET(uint32_t), n = R_GET(uint32_t); r_vkCmdSetScissor(cb, f, n, r_take(&rd, sizeof(VkRect2D) * n)); break; }
    case W_BLEND: { const float *c = r_take(&rd, 16); r_vkCmdSetBlendConstants(cb, c); break; }
    case W_ST_REF: { VkStencilFaceFlags f = R_GET(uint32_t); uint32_t v = R_GET(uint32_t); r_vkCmdSetStencilReference(cb, f, v); break; }
    case W_ST_CMP: { VkStencilFaceFlags f = R_GET(uint32_t); uint32_t v = R_GET(uint32_t); r_vkCmdSetStencilCompareMask(cb, f, v); break; }
    case W_ST_WR: { VkStencilFaceFlags f = R_GET(uint32_t); uint32_t v = R_GET(uint32_t); r_vkCmdSetStencilWriteMask(cb, f, v); break; }
    case W_DEPTH_BIAS: { float a = R_GET(float), b = R_GET(float), c = R_GET(float); r_vkCmdSetDepthBias(cb, a, b, c); break; }
    case W_LINE_W: { float w = R_GET(float); r_vkCmdSetLineWidth(cb, w); break; }
    case W_DRAW: { uint32_t a = R_GET(uint32_t), b = R_GET(uint32_t), c = R_GET(uint32_t), d = R_GET(uint32_t); r_vkCmdDraw(cb, a, b, c, d); break; }
    case W_PUSH: { VkPipelineLayout l = R_GET(VkPipelineLayout); VkShaderStageFlags s = R_GET(uint32_t); uint32_t o = R_GET(uint32_t), n = R_GET(uint32_t); r_vkCmdPushConstants(cb, l, s, o, n, r_take(&rd, n)); break; }
    case W_DRAW_IDX: { uint32_t a = R_GET(uint32_t), b = R_GET(uint32_t), c = R_GET(uint32_t); int32_t d = R_GET(int32_t); uint32_t e = R_GET(uint32_t); r_vkCmdDrawIndexed(cb, a, b, c, d, e); break; }
    case W_DRAW_IND: { VkBuffer b = R_GET(VkBuffer); VkDeviceSize o = R_GET(VkDeviceSize); uint32_t n = R_GET(uint32_t), s = R_GET(uint32_t); r_vkCmdDrawIndirect(cb, b, o, n, s); break; }
    case W_DRAW_IDX_IND: { VkBuffer b = R_GET(VkBuffer); VkDeviceSize o = R_GET(VkDeviceSize); uint32_t n = R_GET(uint32_t), s = R_GET(uint32_t); r_vkCmdDrawIndexedIndirect(cb, b, o, n, s); break; }
    case W_DISPATCH: { uint32_t x = R_GET(uint32_t), y = R_GET(uint32_t), z = R_GET(uint32_t); r_vkCmdDispatch(cb, x, y, z); break; }
    case W_DISPATCH_IND: { VkBuffer b = R_GET(VkBuffer); VkDeviceSize o = R_GET(VkDeviceSize); r_vkCmdDispatchIndirect(cb, b, o); break; }
    case W_RESET_QP: { VkQueryPool p = R_GET(VkQueryPool); uint32_t f = R_GET(uint32_t), n = R_GET(uint32_t); r_vkCmdResetQueryPool(cb, p, f, n); break; }
    case W_TIMESTAMP: { VkPipelineStageFlagBits s = R_GET(uint32_t); VkQueryPool p = R_GET(VkQueryPool); uint32_t q = R_GET(uint32_t); r_vkCmdWriteTimestamp(cb, s, p, q); break; }
    case W_XFB_BIND: { uint32_t f = R_GET(uint32_t), n = R_GET(uint32_t), hs = R_GET(uint32_t); const VkBuffer *b = r_take(&rd, sizeof *b * n); const VkDeviceSize *o = r_take(&rd, sizeof *o * n);
        r_vkCmdBindTransformFeedbackBuffersEXT(cb, f, n, b, o, hs ? r_take(&rd, sizeof(VkDeviceSize) * n) : NULL); break; }
    case W_XFB_BEGIN: case W_XFB_END: { uint32_t f = R_GET(uint32_t), n = R_GET(uint32_t), hb = R_GET(uint32_t), ho = R_GET(uint32_t);
        const VkBuffer *b = hb ? r_take(&rd, sizeof(VkBuffer) * n) : NULL; const VkDeviceSize *o = ho ? r_take(&rd, sizeof(VkDeviceSize) * n) : NULL;
        if (op == W_XFB_BEGIN) r_vkCmdBeginTransformFeedbackEXT(cb, f, n, b, o); else r_vkCmdEndTransformFeedbackEXT(cb, f, n, b, o); break; }
    case W_BEGIN_CB: { VkCommandBufferBeginInfo bi = R_GET(VkCommandBufferBeginInfo); bi.pNext = NULL; bi.pInheritanceInfo = NULL;
        if (r_vkBeginCommandBuffer(cb, &bi)) VKLOG("[RED] worker: vkBeginCommandBuffer failed"); break; }
    default: die("[vk] worker queue: unknown record %d", op);
    }
    *r_ = rd;
}

static void *worker_main(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk record");
    wblk *b = NULL; size_t at = 0;
    pthread_mutex_lock(&g_wq_mu);
    for (;;) {
        if (!b) { b = g_wq_head; at = 0; }
        if (b && at < b->used) {   // records to run
            size_t end = b->used; g_wq_busy = 1; pthread_mutex_unlock(&g_wq_mu);
            wrd r = {blk_data(b) + at}; uint64_t n = 0;
            while ((size_t)(r.p - blk_data(b)) < end) { run_one(&r); n++; }
            at = end;
            pthread_mutex_lock(&g_wq_mu); g_wq_ran += n; continue;
        }
        if (b && b->closed && at >= b->used) {   // a block run out whose writer moved on: back to the free list
            g_wq_head = b->next; b->next = g_wq_free; g_wq_free = b; b = NULL; continue;
        }
        g_wq_busy = 0; pthread_cond_broadcast(&g_wq_idle); pthread_cond_wait(&g_wq_cv, &g_wq_mu);
    }
    return NULL;
}

// Everything queued so far has been run (before a vkEndCommandBuffer or vkQueueSubmit on the calling thread)
void vk_worker_drain(void) {
    if (!g_wq_on) return;
    w_publish();
    pthread_mutex_lock(&g_wq_mu);
    while (g_wq_ran != g_wq_pub || g_wq_busy) { pthread_cond_signal(&g_wq_cv); pthread_cond_wait(&g_wq_idle, &g_wq_mu); }
    pthread_mutex_unlock(&g_wq_mu);
}

// The recorders: a slot's command buffer through the queue, any other straight to the driver
// W_HEAD: the record's op and command buffer, after reserving room for it (16 bytes, then 8 for each field and WA(n) for each array, given as the bound)
#define W_HEAD(op, bound, ...) if (!queued(cb)) { r_##op(cb, __VA_ARGS__); return; } w_reserve(16 + (bound)); W_PUT(uint64_t, (uint64_t)op##_ID); W_PUT(VkCommandBuffer, cb)
#define vkCmdPipelineBarrier_ID W_BARRIER
static VKAPI_ATTR void VKAPI_CALL w_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m,
                                                          uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    W_HEAD(vkCmdPipelineBarrier, 8 * 6 + WA(sizeof *m * nm) + WA(sizeof *b * nb) + WA(sizeof *im * ni), s, d, f, nm, m, nb, b, ni, im);
    W_PUT(uint32_t, s); W_PUT(uint32_t, d); W_PUT(uint32_t, f); W_PUT(uint32_t, nm); W_PUT(uint32_t, nb); W_PUT(uint32_t, ni);
    w_copy(m, sizeof *m * nm); w_copy(b, sizeof *b * nb); w_copy(im, sizeof *im * ni); w_end();
}
#define vkCmdCopyBuffer_ID W_COPY_BUF
static VKAPI_ATTR void VKAPI_CALL w_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) {
    W_HEAD(vkCmdCopyBuffer, 8 * 3 + WA(sizeof *r * n), s, d, n, r); W_PUT(VkBuffer, s); W_PUT(VkBuffer, d); W_PUT(uint32_t, n); w_copy(r, sizeof *r * n); w_end(); }
#define vkCmdCopyBufferToImage_ID W_COPY_B2I
static VKAPI_ATTR void VKAPI_CALL w_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer s, VkImage d, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) {
    W_HEAD(vkCmdCopyBufferToImage, 8 * 4 + WA(sizeof *r * n), s, d, l, n, r); W_PUT(VkBuffer, s); W_PUT(VkImage, d); W_PUT(uint32_t, l); W_PUT(uint32_t, n); w_copy(r, sizeof *r * n); w_end(); }
#define vkCmdCopyImageToBuffer_ID W_COPY_I2B
static VKAPI_ATTR void VKAPI_CALL w_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage s, VkImageLayout l, VkBuffer d, uint32_t n, const VkBufferImageCopy *r) {
    W_HEAD(vkCmdCopyImageToBuffer, 8 * 4 + WA(sizeof *r * n), s, l, d, n, r); W_PUT(VkImage, s); W_PUT(uint32_t, l); W_PUT(VkBuffer, d); W_PUT(uint32_t, n); w_copy(r, sizeof *r * n); w_end(); }
#define vkCmdCopyImage_ID W_COPY_IMG
static VKAPI_ATTR void VKAPI_CALL w_vkCmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    W_HEAD(vkCmdCopyImage, 8 * 5 + WA(sizeof *r * n), s, sl, d, dl, n, r); W_PUT(VkImage, s); W_PUT(uint32_t, sl); W_PUT(VkImage, d); W_PUT(uint32_t, dl); W_PUT(uint32_t, n); w_copy(r, sizeof *r * n); w_end(); }
#define vkCmdBlitImage_ID W_BLIT
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    W_HEAD(vkCmdBlitImage, 8 * 6 + WA(sizeof *r * n), s, sl, d, dl, n, r, f); W_PUT(VkImage, s); W_PUT(uint32_t, sl); W_PUT(VkImage, d); W_PUT(uint32_t, dl); W_PUT(uint32_t, n); W_PUT(uint32_t, f); w_copy(r, sizeof *r * n); w_end(); }
#define vkCmdFillBuffer_ID W_FILL
static VKAPI_ATTR void VKAPI_CALL w_vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize n, uint32_t v) {
    W_HEAD(vkCmdFillBuffer, 8 * 4, b, o, n, v); W_PUT(VkBuffer, b); W_PUT(VkDeviceSize, o); W_PUT(VkDeviceSize, n); W_PUT(uint32_t, v); w_end(); }
#define vkCmdUpdateBuffer_ID W_UPDATE
static VKAPI_ATTR void VKAPI_CALL w_vkCmdUpdateBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize n, const void *p) {
    W_HEAD(vkCmdUpdateBuffer, 8 * 3 + WA(n), b, o, n, p); W_PUT(VkBuffer, b); W_PUT(VkDeviceSize, o); W_PUT(VkDeviceSize, n); w_copy(p, (size_t)n); w_end(); }
#define vkCmdClearColorImage_ID W_CLEAR_COLOR
static VKAPI_ATTR void VKAPI_CALL w_vkCmdClearColorImage(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *r) {
    W_HEAD(vkCmdClearColorImage, 8 * 3 + WA(sizeof *c) + WA(sizeof *r * n), im, l, c, n, r); W_PUT(VkImage, im); W_PUT(uint32_t, l); W_PUT(VkClearColorValue, *c); W_PUT(uint32_t, n); w_copy(r, sizeof *r * n); w_end(); }
#define vkCmdClearDepthStencilImage_ID W_CLEAR_DS
static VKAPI_ATTR void VKAPI_CALL w_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage im, VkImageLayout l, const VkClearDepthStencilValue *c, uint32_t n, const VkImageSubresourceRange *r) {
    W_HEAD(vkCmdClearDepthStencilImage, 8 * 3 + WA(sizeof *c) + WA(sizeof *r * n), im, l, c, n, r); W_PUT(VkImage, im); W_PUT(uint32_t, l); W_PUT(VkClearDepthStencilValue, *c); W_PUT(uint32_t, n); w_copy(r, sizeof *r * n); w_end(); }
#define vkCmdClearAttachments_ID W_CLEAR_ATT
static VKAPI_ATTR void VKAPI_CALL w_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *r) {
    W_HEAD(vkCmdClearAttachments, 8 * 2 + WA(sizeof *a * na) + WA(sizeof *r * nr), na, a, nr, r); W_PUT(uint32_t, na); W_PUT(uint32_t, nr); w_copy(a, sizeof *a * na); w_copy(r, sizeof *r * nr); w_end(); }
#define vkCmdResolveImage_ID W_RESOLVE
static VKAPI_ATTR void VKAPI_CALL w_vkCmdResolveImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve *r) {
    W_HEAD(vkCmdResolveImage, 8 * 5 + WA(sizeof *r * n), s, sl, d, dl, n, r); W_PUT(VkImage, s); W_PUT(uint32_t, sl); W_PUT(VkImage, d); W_PUT(uint32_t, dl); W_PUT(uint32_t, n); w_copy(r, sizeof *r * n); w_end(); }
#define vkCmdBeginRenderPass_ID W_BEGIN_RP
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c) {
    W_HEAD(vkCmdBeginRenderPass, WA(sizeof *bi) + 8 * 2 + WA(sizeof(VkClearValue) * bi->clearValueCount) + WA(sizeof(VkRenderPassAttachmentBeginInfo)) + 8 * 16, bi, c);
    const VkRenderPassAttachmentBeginInfo *abi = NULL;
    for (const VkBaseInStructure *p = bi->pNext; p; p = p->pNext) if (p->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) abi = (const void *)p;
        else VKRED_ONCE(110, "worker: render pass begin with a pNext of type %d, left out", p->sType);
    W_PUT(VkRenderPassBeginInfo, *bi); W_PUT(uint32_t, c); W_PUT(uint32_t, abi != NULL); w_copy(bi->pClearValues, sizeof(VkClearValue) * bi->clearValueCount);
    if (abi) { W_PUT(VkRenderPassAttachmentBeginInfo, *abi); w_copy(abi->pAttachments, sizeof(VkImageView) * abi->attachmentCount); }
    w_end();
}
#define vkCmdEndRenderPass_ID W_END_RP
static VKAPI_ATTR void VKAPI_CALL w_vkCmdEndRenderPass(VkCommandBuffer cb) { if (!queued(cb)) { r_vkCmdEndRenderPass(cb); return; } w_reserve(16); W_PUT(uint64_t, W_END_RP); W_PUT(VkCommandBuffer, cb); w_end(); }
#define vkCmdBeginRendering_ID W_BEGIN_R
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    W_HEAD(vkCmdBeginRendering, WA(sizeof *ri) + 8 * 2 + WA(sizeof(VkRenderingAttachmentInfo) * (ri->colorAttachmentCount + 2)), ri);
    if (ri->pNext) VKRED_ONCE(111, "worker: rendering info with a pNext, left out");
    W_PUT(VkRenderingInfo, *ri); W_PUT(uint32_t, ri->pDepthAttachment != NULL); W_PUT(uint32_t, ri->pStencilAttachment != NULL);
    w_copy(ri->pColorAttachments, sizeof(VkRenderingAttachmentInfo) * ri->colorAttachmentCount);
    if (ri->pDepthAttachment) w_copy(ri->pDepthAttachment, sizeof(VkRenderingAttachmentInfo));
    if (ri->pStencilAttachment) w_copy(ri->pStencilAttachment, sizeof(VkRenderingAttachmentInfo));
    w_end();
}
#define vkCmdEndRendering_ID W_END_R
static VKAPI_ATTR void VKAPI_CALL w_vkCmdEndRendering(VkCommandBuffer cb) { if (!queued(cb)) { r_vkCmdEndRendering(cb); return; } w_reserve(16); W_PUT(uint64_t, W_END_R); W_PUT(VkCommandBuffer, cb); w_end(); }
#define vkCmdBindPipeline_ID W_BIND_PIPE
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) { W_HEAD(vkCmdBindPipeline, 8 * 2, bp, p); W_PUT(uint32_t, bp); W_PUT(VkPipeline, p); w_end(); }
#define vkCmdBindDescriptorSets_ID W_BIND_SETS
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) {
    W_HEAD(vkCmdBindDescriptorSets, 8 * 5 + WA(sizeof *s * n) + WA(4 * (size_t)nd), bp, l, first, n, s, nd, d);
    W_PUT(uint32_t, bp); W_PUT(VkPipelineLayout, l); W_PUT(uint32_t, first); W_PUT(uint32_t, n); W_PUT(uint32_t, nd); w_copy(s, sizeof *s * n); w_copy(d, 4 * (size_t)nd); w_end();
}
#define vkCmdBindVertexBuffers_ID W_BIND_VB
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    W_HEAD(vkCmdBindVertexBuffers, 8 * 2 + WA(sizeof *b * n) + WA(sizeof *o * n), first, n, b, o); W_PUT(uint32_t, first); W_PUT(uint32_t, n); w_copy(b, sizeof *b * n); w_copy(o, sizeof *o * n); w_end(); }
#define vkCmdBindIndexBuffer_ID W_BIND_IB
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) { W_HEAD(vkCmdBindIndexBuffer, 8 * 3, b, o, t); W_PUT(VkBuffer, b); W_PUT(VkDeviceSize, o); W_PUT(uint32_t, t); w_end(); }
#define vkCmdSetViewport_ID W_VIEWPORT
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetViewport(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkViewport *v) { W_HEAD(vkCmdSetViewport, 8 * 2 + WA(sizeof *v * n), f, n, v); W_PUT(uint32_t, f); W_PUT(uint32_t, n); w_copy(v, sizeof *v * n); w_end(); }
#define vkCmdSetScissor_ID W_SCISSOR
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetScissor(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkRect2D *s) { W_HEAD(vkCmdSetScissor, 8 * 2 + WA(sizeof *s * n), f, n, s); W_PUT(uint32_t, f); W_PUT(uint32_t, n); w_copy(s, sizeof *s * n); w_end(); }
#define vkCmdSetBlendConstants_ID W_BLEND
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) { W_HEAD(vkCmdSetBlendConstants, 16, c); w_copy(c, 16); w_end(); }
#define vkCmdSetStencilReference_ID W_ST_REF
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) { W_HEAD(vkCmdSetStencilReference, 8 * 2, f, v); W_PUT(uint32_t, f); W_PUT(uint32_t, v); w_end(); }
#define vkCmdSetStencilCompareMask_ID W_ST_CMP
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) { W_HEAD(vkCmdSetStencilCompareMask, 8 * 2, f, v); W_PUT(uint32_t, f); W_PUT(uint32_t, v); w_end(); }
#define vkCmdSetStencilWriteMask_ID W_ST_WR
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) { W_HEAD(vkCmdSetStencilWriteMask, 8 * 2, f, v); W_PUT(uint32_t, f); W_PUT(uint32_t, v); w_end(); }
#define vkCmdSetDepthBias_ID W_DEPTH_BIAS
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetDepthBias(VkCommandBuffer cb, float a, float b, float c) { W_HEAD(vkCmdSetDepthBias, 8 * 3, a, b, c); W_PUT(float, a); W_PUT(float, b); W_PUT(float, c); w_end(); }
#define vkCmdSetLineWidth_ID W_LINE_W
static VKAPI_ATTR void VKAPI_CALL w_vkCmdSetLineWidth(VkCommandBuffer cb, float w) { W_HEAD(vkCmdSetLineWidth, 8, w); W_PUT(float, w); w_end(); }
#define vkCmdDraw_ID W_DRAW
static VKAPI_ATTR void VKAPI_CALL w_vkCmdDraw(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { W_HEAD(vkCmdDraw, 8 * 4, a, b, c, d); W_PUT(uint32_t, a); W_PUT(uint32_t, b); W_PUT(uint32_t, c); W_PUT(uint32_t, d); w_end(); }
#define vkCmdPushConstants_ID W_PUSH
static VKAPI_ATTR void VKAPI_CALL w_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags s, uint32_t o, uint32_t n, const void *p) {
    W_HEAD(vkCmdPushConstants, 8 * 4 + WA(n), l, s, o, n, p); W_PUT(VkPipelineLayout, l); W_PUT(uint32_t, s); W_PUT(uint32_t, o); W_PUT(uint32_t, n); w_copy(p, n); w_end(); }
#define vkCmdDrawIndexed_ID W_DRAW_IDX
static VKAPI_ATTR void VKAPI_CALL w_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) {
    W_HEAD(vkCmdDrawIndexed, 8 * 5, a, b, c, d, e); W_PUT(uint32_t, a); W_PUT(uint32_t, b); W_PUT(uint32_t, c); W_PUT(int32_t, d); W_PUT(uint32_t, e); w_end(); }
#define vkCmdDrawIndirect_ID W_DRAW_IND
static VKAPI_ATTR void VKAPI_CALL w_vkCmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) { W_HEAD(vkCmdDrawIndirect, 8 * 4, b, o, n, s); W_PUT(VkBuffer, b); W_PUT(VkDeviceSize, o); W_PUT(uint32_t, n); W_PUT(uint32_t, s); w_end(); }
#define vkCmdDrawIndexedIndirect_ID W_DRAW_IDX_IND
static VKAPI_ATTR void VKAPI_CALL w_vkCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) { W_HEAD(vkCmdDrawIndexedIndirect, 8 * 4, b, o, n, s); W_PUT(VkBuffer, b); W_PUT(VkDeviceSize, o); W_PUT(uint32_t, n); W_PUT(uint32_t, s); w_end(); }
#define vkCmdDispatch_ID W_DISPATCH
static VKAPI_ATTR void VKAPI_CALL w_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) { W_HEAD(vkCmdDispatch, 8 * 3, x, y, z); W_PUT(uint32_t, x); W_PUT(uint32_t, y); W_PUT(uint32_t, z); w_end(); }
#define vkCmdDispatchIndirect_ID W_DISPATCH_IND
static VKAPI_ATTR void VKAPI_CALL w_vkCmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o) { W_HEAD(vkCmdDispatchIndirect, 8 * 2, b, o); W_PUT(VkBuffer, b); W_PUT(VkDeviceSize, o); w_end(); }
#define vkCmdResetQueryPool_ID W_RESET_QP
static VKAPI_ATTR void VKAPI_CALL w_vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool p, uint32_t f, uint32_t n) { W_HEAD(vkCmdResetQueryPool, 8 * 3, p, f, n); W_PUT(VkQueryPool, p); W_PUT(uint32_t, f); W_PUT(uint32_t, n); w_end(); }
#define vkCmdWriteTimestamp_ID W_TIMESTAMP
static VKAPI_ATTR void VKAPI_CALL w_vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits s, VkQueryPool p, uint32_t q) { W_HEAD(vkCmdWriteTimestamp, 8 * 3, s, p, q); W_PUT(uint32_t, s); W_PUT(VkQueryPool, p); W_PUT(uint32_t, q); w_end(); }
#define vkCmdBindTransformFeedbackBuffersEXT_ID W_XFB_BIND
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBindTransformFeedbackBuffersEXT(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkBuffer *b, const VkDeviceSize *o, const VkDeviceSize *sz) {
    W_HEAD(vkCmdBindTransformFeedbackBuffersEXT, 8 * 3 + 3 * WA(sizeof(VkDeviceSize) * n), f, n, b, o, sz); W_PUT(uint32_t, f); W_PUT(uint32_t, n); W_PUT(uint32_t, sz != NULL);
    w_copy(b, sizeof *b * n); w_copy(o, sizeof *o * n); if (sz) w_copy(sz, sizeof *sz * n); w_end(); }
#define vkCmdBeginTransformFeedbackEXT_ID W_XFB_BEGIN
static VKAPI_ATTR void VKAPI_CALL w_vkCmdBeginTransformFeedbackEXT(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    W_HEAD(vkCmdBeginTransformFeedbackEXT, 8 * 4 + 2 * WA(sizeof(VkDeviceSize) * n), f, n, b, o); W_PUT(uint32_t, f); W_PUT(uint32_t, n); W_PUT(uint32_t, b != NULL); W_PUT(uint32_t, o != NULL);
    if (b) w_copy(b, sizeof *b * n); if (o) w_copy(o, sizeof *o * n); w_end(); }
#define vkCmdEndTransformFeedbackEXT_ID W_XFB_END
static VKAPI_ATTR void VKAPI_CALL w_vkCmdEndTransformFeedbackEXT(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    W_HEAD(vkCmdEndTransformFeedbackEXT, 8 * 4 + 2 * WA(sizeof(VkDeviceSize) * n), f, n, b, o); W_PUT(uint32_t, f); W_PUT(uint32_t, n); W_PUT(uint32_t, b != NULL); W_PUT(uint32_t, o != NULL);
    if (b) w_copy(b, sizeof *b * n); if (o) w_copy(o, sizeof *o * n); w_end(); }
static VKAPI_ATTR VkResult VKAPI_CALL w_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
    if (!queued(cb)) return r_vkBeginCommandBuffer(cb, bi);
    if (bi->pNext || bi->pInheritanceInfo) VKRED_ONCE(112, "worker: command buffer begin with a pNext or inheritance, left out");
    w_reserve(16 + WA(sizeof *bi)); W_PUT(uint64_t, W_BEGIN_CB); W_PUT(VkCommandBuffer, cb); W_PUT(VkCommandBufferBeginInfo, *bi); w_end(); return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_vkEndCommandBuffer(VkCommandBuffer cb) { if (queued(cb)) vk_worker_drain(); return r_vkEndCommandBuffer(cb); }
static VKAPI_ATTR VkResult VKAPI_CALL w_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {   // drained by the thread that recorded a slot's command buffers;
    int mine = 0;                                                                                                    // another thread's submission (the present's) leaves the queue alone
    for (uint32_t i = 0; i < n && !mine; i++) for (uint32_t k = 0; k < s[i].commandBufferCount; k++) if (queued(s[i].pCommandBuffers[k])) { mine = 1; break; }
    if (mine) vk_worker_drain();
    return r_vkQueueSubmit(q, n, s, f);
}

void vk_worker_install(void) {
    const char *e = getenv("DM_VK_WORKER"); if (!(e && atoi(e))) return;
    #define W_SWAP(f) r_##f = f; f = w_##f;
    W_FNS(W_SWAP)
    g_wq_on = 1;
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED); pthread_create(&t, &a, worker_main, NULL); pthread_attr_destroy(&a);
    VKLOG("Vulkan commands recorded on a worker thread (DM_VK_WORKER)");
}
