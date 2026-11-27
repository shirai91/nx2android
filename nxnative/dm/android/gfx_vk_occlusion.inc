// ---------------- NVN samples-passed counting (occlusion queries) ----------------
// NVN's SAMPLES_PASSED is one cumulative counter serving every queue: ResetCounter zeroes it, every draw afterwards adds the samples that pass the depth and
// stencil tests (clears add nothing), and ReportCounter writes {count, ticks} when the GPU completes. Two reports with no reset between keep counting, so the
// second holds the first's samples too. The Metal path counts through visibility result buffers (nvn_mtl.m "Occlusion queries"); here the count comes from
// Vulkan occlusion queries, one per run of draws between two counter ops inside a render pass, rather than one per draw: a pass's draws count into the query
// begun at its first counted draw, and a report or reset inside the pass closes it so the next draw begins another. The cost therefore follows the counter
// ops, not the draws -- measured on FINAL FANTASY RESONANCE: a dungeon of 87 passes and 996 draws a frame began 139 queries, a hall of 64 passes and 1425
// draws began 563 (506 reports a frame). A query lives inside one render pass instance, so it also ends at the pass end.
// Nothing waits for the GPU. A report's segment (its queries, plus the draws that got none) goes to the completion thread with its submission's fence; there the
// results are added to the running count in submission order, written out, and the queries go back to the ring. The replay thread owns the record-side state
// (head, seg0, lost, open, counting); the completion thread owns the count and the ring's ready mark, which the replay reads atomically.
// What is not known reads as visible: a draw that got no query (the ring was full), a draw the replay skipped, and a result still unavailable after its fence
// each count one sample. A draw a render condition skipped counts nothing, as on the GPU. The count therefore only culls what was positively measured empty,
// never what we failed to measure -- the opposite of the constant this replaces, which read every query as visible and so culled nothing at all.
// A report with no reset before it writes the constant (its count is not defined) and starts counting from there, as the GPU's counter runs from the queue's
// creation; a game that never resets reads the difference of two reports around each query.
#ifndef DM_VK_OCCLUSION_INC
#define DM_VK_OCCLUSION_INC
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint64_t q0; uint32_t nq, lost, fresh; } oq_seg;   // a report's queries by sequence number, the draws of it that count one sample without a query, and a reset since the last report

static struct {
    int on, counting, open;
    uint32_t ring;
    uint64_t head, seg0;          // the next query's sequence number; the segment's first
    uint32_t lost, fresh;         // draws of this segment counted one sample each (no query, or skipped by the replay); a reset since the last report
    uint64_t acc;                 // the running count (completion thread)
    _Atomic uint64_t ready;       // queries below it are reset and may begin (the completion thread moves it on)
    uint64_t s_reset, s_rep, s_const, s_q, s_lost, s_unav, s_zero, s_fly;
} g_oq;

static void vk_oq_init(int on, uint32_t ring) {
    memset(&g_oq, 0, sizeof g_oq);
    g_oq.on = on; g_oq.ring = ring ? ring : 1;
    atomic_store(&g_oq.ready, g_oq.ring);
}
static int vk_oq_on(void) { return g_oq.on; }

// A counted draw: the query index to begin, or -1 when none is needed (one already open in this pass, not counting, or the ring is full and the draw counts 1).
// The open test comes first: it is the common case of every draw but a pass's first
static int vk_oq_draw(void) {
    if (g_oq.open || !g_oq.on || !g_oq.counting) return -1;
    if (g_oq.head >= atomic_load_explicit(&g_oq.ready, memory_order_acquire)) { g_oq.lost++; g_oq.s_lost++; return -1; }
    uint64_t fly = g_oq.head + 1 - (atomic_load_explicit(&g_oq.ready, memory_order_relaxed) - g_oq.ring);
    if (fly > g_oq.s_fly) g_oq.s_fly = fly;
    g_oq.open = 1; g_oq.s_q++;
    return (int)(g_oq.head++ % g_oq.ring);
}
// The open query's index to end, or -1 when none is open. Called at the pass end and before a report or reset inside the pass
static int vk_oq_close(void) {
    if (!g_oq.open) return -1;
    g_oq.open = 0;
    return (int)((g_oq.head - 1) % g_oq.ring);
}
// A draw the replay did not record while counting (the walker compares the draw count across the whole draw op): what is not known reads as visible
static void vk_oq_lost(void) { if (g_oq.on && g_oq.counting) { g_oq.lost++; g_oq.s_lost++; } }

// Every reset counts. There is no look-ahead over the record stream to skip a segment no report would read: the walker's execution order follows OP_JUMP and
// OP_CALL, so the linear bytes after an op are not necessarily records, and scanning them without the buffer's end walked into unmapped memory and crashed
// DRAGON QUEST VII Reimagined (SIGSEGV in the scan, twice). The games measured pair every reset with a report, so nothing is counted that no report reads.
// OP_RESET_COUNTER(SAMPLES_PASSED): the queries since the last report go back unread. The count starts from zero at the next report (fresh), not here: nothing
// between a reset and that report reads the count, so one post op serves both and a reset with no queries needs none at all
static void vk_oq_reset(oq_seg *s) {
    s->q0 = g_oq.seg0; s->nq = (uint32_t)(g_oq.head - g_oq.seg0); s->lost = 0; s->fresh = 0;
    g_oq.seg0 = g_oq.head; g_oq.lost = 0; g_oq.fresh = 1; g_oq.counting = 1; g_oq.s_reset++;
}
// OP_REPORT_COUNTER(SAMPLES_PASSED): 1 = the segment is counted (post it with the submission's fence), 0 = write the constant
static int vk_oq_report(oq_seg *s) {
    g_oq.s_rep++;
    if (!g_oq.on) return 0;
    if (!g_oq.counting) { g_oq.counting = 1; g_oq.seg0 = g_oq.head; g_oq.lost = 0; g_oq.fresh = 0; g_oq.s_const++; return 0; }
    s->q0 = g_oq.seg0; s->nq = (uint32_t)(g_oq.head - g_oq.seg0); s->lost = g_oq.lost; s->fresh = g_oq.fresh;
    g_oq.seg0 = g_oq.head; g_oq.lost = 0; g_oq.fresh = 0;
    return 1;
}

// ---- completion thread, in submission order ----
// Give queries back to the ring: after their results were read, or unread at a reset. A slot frees the sequence one ring past it, so the mark is the end plus
// the ring, and the replay may take up to that many (in order, so the mark only moves on)
static void vk_oq_give(uint64_t q0, uint32_t nq) {
    if (nq) atomic_store_explicit(&g_oq.ready, q0 + nq + g_oq.ring, memory_order_release);
}
// Add a report's segment to the running count and return the value to write. samples: the sum of the results that were available after the fence; unav: how
// many were not, each counting one sample, as does every draw of the segment that got no query
static uint64_t vk_oq_add(const oq_seg *s, uint64_t samples, uint32_t unav) {
    uint64_t n = s->lost + samples + unav;
    g_oq.s_unav += unav;
    if (s->fresh) g_oq.acc = 0;   // the reset since the last report: its zero rides this report, in order
    g_oq.acc += n;
    if (!n) g_oq.s_zero++;
    return g_oq.acc;
}
// The statistics line, or 0 when the game resets and reports none. frames: the window's frame count
static int vk_oq_stats(char *b, size_t n, double frames) {
    if (!g_oq.s_reset && !g_oq.s_rep) return 0;
    double f = frames > 0 ? frames : 1;
    return snprintf(b, n, "occlusion queries per frame: %.1f resets, %.1f reports (%.1f written as a constant, %.1f read 0 samples), %.1f queries begun, "
            "%.1f draws counted 1 without a query, %.1f results unavailable, %llu most in flight",
            g_oq.s_reset / f, g_oq.s_rep / f, g_oq.s_const / f, g_oq.s_zero / f, g_oq.s_q / f, g_oq.s_lost / f, g_oq.s_unav / f,
            (unsigned long long)g_oq.s_fly);
}
static void vk_oq_stats_clear(void) { g_oq.s_reset = g_oq.s_rep = g_oq.s_const = g_oq.s_q = g_oq.s_lost = g_oq.s_unav = g_oq.s_zero = g_oq.s_fly = 0; }
#endif