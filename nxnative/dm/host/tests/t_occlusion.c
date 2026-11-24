// NVN samples-passed counting (dm/android/gfx_vk_occlusion.inc): the cumulative counter's semantics, the ring's accounting and the visible-when-unknown
// fallbacks, with no Vulkan and no guest. The queries are stood in for by a sample count per sequence number, as the completion thread reads them
#include "../../android/gfx_vk_occlusion.inc"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

enum { RING = 8, SEQMAX = 4096 };
static uint64_t g_samples[SEQMAX], g_seq;   // what the GPU would have counted for each query, by sequence number

// One render pass: its first counted draw begins the query, the rest count into it, the pass end closes it. samples: what the whole pass counted
static void pass(uint64_t samples, int ndraw) {
    int begun = 0;
    for (int i = 0; i < ndraw; i++) if (vk_oq_draw() >= 0) { begun = 1; g_seq++; }
    vk_oq_close();
    if (begun) g_samples[g_seq - 1] = samples;
}
static uint32_t reset(void) {   // the queries to give back unread: 0 means the reset needs no post op of its own (its zero rides the next report)
    oq_seg s; vk_oq_close(); vk_oq_reset(&s);
    vk_oq_give(s.q0, s.nq);
    return s.nq;
}
// A report: 1 counted (its segment added to the running count), 0 the constant. navail: how many of the segment's results were available after the fence
static uint64_t report(uint32_t navail, int *counted) {
    oq_seg s; vk_oq_close();
    *counted = vk_oq_report(&s);
    if (!*counted) return 1;
    uint64_t sum = 0; uint32_t unav = 0;
    for (uint32_t i = 0; i < s.nq; i++) { if (i < navail) sum += g_samples[s.q0 + i]; else unav++; }
    uint64_t v = vk_oq_add(&s, sum, unav);
    vk_oq_give(s.q0, s.nq);
    return v;
}
static void fresh(int on) { vk_oq_init(on, RING); g_seq = 0; memset(g_samples, 0, sizeof g_samples); }

int main(void) {
    int c;
    // R1: reset zeroes, the draws after it add, the report writes the count. One query per pass, not per draw
    fresh(1);
    reset(); pass(100, 400); pass(250, 300);
    CHECK(g_seq == 2, "two passes of 700 draws cost two queries");
    CHECK(report(2, &c) == 350 && c, "a report writes the samples of both passes");

    // R2: two reports with no reset between keep counting, so the second holds the first's samples too
    fresh(1);
    reset(); pass(100, 10);
    CHECK(report(1, &c) == 100, "the first report holds its own pass");
    pass(40, 10);
    CHECK(report(1, &c) == 140, "the second report holds both passes");
    reset(); pass(7, 10);
    CHECK(report(1, &c) == 7, "a reset starts the count from zero again");

    // R3: a report with no reset before it writes the constant and starts counting from there
    fresh(1);
    CHECK(report(0, &c) == 1 && !c, "a report before any reset writes the constant");
    pass(60, 10);
    CHECK(report(1, &c) == 60 && c, "counting started at that report");

    // R5: what is not known reads as visible -- a result unavailable after its fence, and a draw the replay skipped
    fresh(1);
    reset(); pass(100, 10); pass(900, 10);
    CHECK(report(1, &c) == 101, "a result unavailable after the fence counts one sample, not zero");
    fresh(1);
    reset(); pass(100, 10); vk_oq_lost(); vk_oq_lost();
    CHECK(report(1, &c) == 102, "two draws the replay skipped count one sample each");

    // R5: the ring full counts one sample per draw rather than dropping it
    fresh(1);
    reset();
    for (int i = 0; i < RING; i++) pass(10, 5);
    CHECK(g_seq == RING, "the ring holds one query per pass");
    for (int i = 0; i < 3; i++) pass(10, 5);   // no completion yet: the ring is full
    CHECK(g_seq == RING, "a full ring begins no more queries");
    CHECK(report(RING, &c) == RING * 10 + 15, "the fifteen draws with no query count one sample each");

    // the ring recycles: after the queries go back, later passes begin them again
    pass(20, 5); pass(30, 5);
    CHECK(g_seq == RING + 2, "given-back slots are taken again");
    CHECK(report(2, &c) == RING * 10 + 15 + 50, "the recycled queries count their own samples");

    // the reset's zero rides the next report, so a reset with no queries of its own needs no post op
    fresh(1);
    reset(); pass(100, 10); report(1, &c);
    CHECK(reset() == 0, "a reset right after a report has no queries to give back");
    pass(50, 10);
    CHECK(report(1, &c) == 50, "the count starts from zero at that report, not at the reset's own op");
    reset(); pass(100, 10); reset(); reset(); pass(7, 10);
    CHECK(report(1, &c) == 7, "two resets with no report between still zero once");

    // a clear adds nothing: no draw, no samples
    fresh(1);
    reset(); pass(0, 0);
    CHECK(report(0, &c) == 0 && c, "a pass of no draws counts zero, and zero is a real count");

    // the count is one running total across submissions: a report in a later submission adds to the earlier one
    fresh(1);
    reset(); pass(100, 10);
    CHECK(report(1, &c) == 100, "the first submission's report");
    pass(25, 10);   // the next submission, no reset
    CHECK(report(1, &c) == 125, "the next submission's report adds to the running count");

    // the open query spans the pass's draws and closes once
    fresh(1);
    reset();
    int q = vk_oq_draw();
    CHECK(q >= 0, "the first counted draw begins a query");
    CHECK(vk_oq_draw() == -1, "a draw while one is open begins no second query");
    CHECK(vk_oq_close() == q, "the pass end closes the query that was begun");
    CHECK(vk_oq_close() == -1, "closing again ends nothing");

    // off: every report writes the constant and no query is begun (DM_VK_OCCLUSION=0, and a device without the pool)
    fresh(0);
    reset(); pass(100, 400);
    CHECK(g_seq == 0, "off begins no query");
    CHECK(report(0, &c) == 1 && !c, "off writes the constant");

    // a game that never reports samples-passed never starts counting
    fresh(1);
    pass(100, 400);
    CHECK(g_seq == 0, "no reset and no report: nothing is counted");

    char b[512];
    CHECK(vk_oq_stats(b, sizeof b, 1) == 0, "no statistics line while the game resets and reports none");
    reset(); pass(10, 5); report(1, &c);
    CHECK(vk_oq_stats(b, sizeof b, 1) > 0, "the statistics line appears once the game uses the counter");

    printf("t_occlusion: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}