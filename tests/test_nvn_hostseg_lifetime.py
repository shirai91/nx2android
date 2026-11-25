"""Host-lent command segments live as long as the list they hang off (seedds/nxnative 0d1037875,
0e409ed8a, bf93977b8).

The harness compiles the real recording code from hle_nvn.c (cut out as in test_nvn_control_memory).
A segment is freed when another recording writes over its list's guest bytes, or tries to write a
record there that does not fit, or when the pool its list was recorded in is finalized, never when
command memory over it is only granted; and grants of 16 bytes or more (Borderlands 3 grants 40 and
56) are recorded into the game's memory.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from host_cc import posix
from test_nvn_control_memory import NVN, c_function, harness


# nvnMemoryPoolFinalize from hle_nvn.c, with the pool bookkeeping it calls besides the host-lent segments stubbed out
FINALIZE = r'''
static void nvn_drain(void) {}
static void vpool_release(pool_t *o) { (void)o; }
static void nvn_dirty_remove_range(const void *mem) { (void)mem; }
static void replay_lock(void) {}
static void replay_unlock(void) {}
static void mtl_pool_del(const void *mem) { (void)mem; }
static void mtl_alias_drop_pool(const void *pool) { (void)pool; }
static void fl_drop(const uint8_t *a, uint64_t n) { (void)a; (void)n; }
static void ft_drop(const uint8_t *a, uint64_t n) { (void)a; (void)n; }
'''


def finalize():
    return FINALIZE + c_function((NVN / 'hle_nvn.c').read_text(encoding='utf-8'), 'NVN(nvnMemoryPoolFinalize,') + '\n'


CHECKS = r'''
static int failed;
#define EXPECT(what, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); \
    if (w_ != g_) { printf("FAIL %s: want %lld, got %lld\n", what, w_, g_); failed++; } } while (0)
enum { NB = 40, TAGOP = 200 };
// A record of the test: 40 bytes, payload (tag, index)
static void rec_one(NVNcommandBuffer *c, uint32_t tag, uint32_t i, uint32_t size) {
    uint8_t *p = cmd_alloc(CB(c), size); put_hdr(p, TAGOP, size); memcpy(p + 8, &tag, 4); memcpy(p + 12, &i, 4);
}
// A list: an optional lead record, an optional big first record (larger than a 64-byte block), then NB records
static NVNcommandHandle rec_list(NVNcommandBuffer *c, int lead, int big, uint32_t tag) {
    h_nvnCommandBufferBeginRecording(c);
    if (lead) rec_one(c, tag, 1000, 24);
    if (big) rec_one(c, tag, 2000, 400);
    for (uint32_t i = 0; i < NB; i++) rec_one(c, tag, i, 40);
    return h_nvnCommandBufferEndRecording(c);
}
// How many of the NB records of tag the list replays in order, following its JUMPs to its END
static int list_ok(NVNcommandHandle h, uint32_t tag) {
    int good = 0; const uint8_t *p = (const uint8_t *)(uintptr_t)h;
    for (int steps = 0; steps < 10000; steps++) {
        op_hdr hd; memcpy(&hd, p, sizeof hd);
        if (hd.op == OP_END) return good;
        if (hd.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; }
        if (hd.op != TAGOP || hd.size < 16) return -1;
        uint32_t t, i; memcpy(&t, p + 8, 4); memcpy(&i, p + 12, 4);
        if (t != tag) return -1;
        if (i < 1000) { if (i != (uint32_t)good) return -1; good++; }
        p += hd.size;
    }
    return -1;
}
static int seg_of(uintptr_t lo) { for (int i = 0; i < g_nhostseg; i++) if (g_hostseg[i].busy && g_hostseg[i].org_lo == lo) return i; return -1; }
static int busy_n(void) { int n = 0; for (int i = 0; i < g_nhostseg; i++) n += g_hostseg[i].busy; return n; }
// Callback handing out 64-byte blocks of g_blk in turn (Pokémon's 1 KB blocks, scaled to the test's records): a record larger than a block takes two of them, then a host-lent segment
static _Alignas(4096) uint8_t g_blkmem[8192]; static size_t g_blkoff; static pool_t g_blk = { .mem = g_blkmem, .size = sizeof g_blkmem };
static void blk_cb(NVNcommandBuffer *c, NVNcommandBufferMemoryEvent ev, size_t minsz, void *data) {
    (void)data; (void)minsz; if (ev != NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY) return;
    h_nvnCommandBufferAddCommandMemory(c, (NVNmemoryPool *)&g_blk, (ptrdiff_t)g_blkoff, 64); g_blkoff += 64;
}
static _Alignas(4096) uint8_t m[8192]; static pool_t mp = { .mem = m, .size = sizeof m };
static NVNmemoryPool *P = (NVNmemoryPool *)&mp;

static void grant_is_no_write(void) {   // 0d1037875
    NVNcommandBuffer ca = {0}, cb = {0}, cx = {0};
    h_nvnCommandBufferInitialize(&ca, NULL); h_nvnCommandBufferInitialize(&cb, NULL); h_nvnCommandBufferInitialize(&cx, NULL);   // no callbacks: an overflow goes to a host segment
    // A, at 512, overflows its 64 bytes; B, before it, is granted 1 KB that reaches over A (max(S, 512)) and writes one record at 0
    h_nvnCommandBufferAddCommandMemory(&ca, P, 512, 64); NVNcommandHandle ha = rec_list(&ca, 0, 0, 0xa);
    EXPECT("A's list hangs off its guest bytes", 1, seg_of((uintptr_t)(m + 512)) >= 0);
    h_nvnCommandBufferAddCommandMemory(&cb, P, 0, 1024); h_nvnCommandBufferBeginRecording(&cb); rec_one(&cb, 0xb, 0, 40); h_nvnCommandBufferEndRecording(&cb);
    EXPECT("a wider grant keeps A's segment", 1, seg_of((uintptr_t)(m + 512)) >= 0);
    h_nvnCommandBufferAddCommandMemory(&cx, P, 4096, 64); rec_list(&cx, 0, 0, 0xdead);   // another overflow, which takes a free segment of the same size
    EXPECT("A replays whole after another buffer's overflow", NB, list_ok(ha, 0xa));
    // A recorded again in place: its old bytes are written over, so its segment is free, and the next overflow takes it instead of a new one
    int a = seg_of((uintptr_t)(m + 512)); uint8_t *a_mem = a >= 0 ? g_hostseg[a].mem : NULL;
    h_nvnCommandBufferAddCommandMemory(&ca, P, 512, 64); h_nvnCommandBufferBeginRecording(&ca); rec_one(&ca, 0xa, 0, 16); h_nvnCommandBufferEndRecording(&ca);
    int a_busy = -1; for (int i = 0; i < g_nhostseg; i++) if (g_hostseg[i].mem == a_mem) a_busy = g_hostseg[i].busy;
    EXPECT("re-recording A in place frees its segment", 0, a_busy);
    int nseg = g_nhostseg;
    h_nvnCommandBufferAddCommandMemory(&cx, P, 6144, 64); rec_list(&cx, 0, 0, 0xdead);
    EXPECT("the next overflow reuses it: no new segment", nseg, g_nhostseg);
    // retire this case's lists
    h_nvnCommandBufferAddCommandMemory(&cx, P, 4096, 64); h_nvnCommandBufferBeginRecording(&cx); h_nvnCommandBufferEndRecording(&cx);
    h_nvnCommandBufferAddCommandMemory(&cx, P, 6144, 64); h_nvnCommandBufferBeginRecording(&cx); h_nvnCommandBufferEndRecording(&cx);
}

static void record_that_does_not_fit(void) {   // 0e409ed8a
    NVNcommandBuffer ca = {0}, cb = {0}, cx = {0}, cv = {0};
    h_nvnCommandBufferInitialize(&ca, NULL); CB(&ca)->cb = blk_cb;
    h_nvnCommandBufferInitialize(&cb, NULL); h_nvnCommandBufferInitialize(&cx, NULL); h_nvnCommandBufferInitialize(&cv, NULL);
    // Each frame the buffer is given the same block, and its first record takes two callback blocks and then a lent segment, writing nothing in the game's memory but JUMPs
    int nseg = -1, good = 1;
    for (int frame = 0; frame < 100; frame++) {
        g_blkoff = 64; h_nvnCommandBufferAddCommandMemory(&ca, (NVNmemoryPool *)&g_blk, 0, 64);
        NVNcommandHandle h = rec_list(&ca, 0, 1, 0x100u + (uint32_t)frame);
        good &= list_ok(h, 0x100u + (uint32_t)frame) == NB;
        if (!frame) nseg = g_nhostseg;
    }
    EXPECT("an overflow before the first record: each frame's list replays whole", 1, good);
    EXPECT("the next attempt in the same blocks takes its segment again: no new one", nseg, g_nhostseg);
    // A's first record overflowed its 64-byte grant at 2048, so nothing is written there; B, the slot before, is granted [1536, 2560) and writes one record at 1536
    h_nvnCommandBufferAddCommandMemory(&cv, P, 2048, 64); NVNcommandHandle ha = rec_list(&cv, 0, 1, 0xa);
    h_nvnCommandBufferAddCommandMemory(&cb, P, 1536, 1024); h_nvnCommandBufferBeginRecording(&cb); rec_one(&cb, 0xb, 0, 40); h_nvnCommandBufferEndRecording(&cb);
    h_nvnCommandBufferAddCommandMemory(&cx, P, 6144, 64); rec_list(&cx, 0, 1, 0xdead);
    EXPECT("a wider grant over an overflowed list's byte keeps its segment", NB, list_ok(ha, 0xa));
    // C wrote a record at 1024 and ran short in the callback blocks at 4096; D re-records 2048, between the two but none of C's
    h_nvnCommandBufferAddCommandMemory(&ca, (NVNmemoryPool *)&g_blk, 1024, 64); g_blkoff = 4096; NVNcommandHandle hc = rec_list(&ca, 1, 1, 0xc);
    h_nvnCommandBufferAddCommandMemory(&cb, (NVNmemoryPool *)&g_blk, 2048, 64); h_nvnCommandBufferBeginRecording(&cb); rec_one(&cb, 0xd, 0, 24); h_nvnCommandBufferEndRecording(&cb);
    h_nvnCommandBufferAddCommandMemory(&cx, P, 7168, 64); rec_list(&cx, 0, 1, 0xdead);
    EXPECT("a write between a list's first block and its callback block keeps its segment", NB, list_ok(hc, 0xc));
}

static void small_grants(void) {   // bf93977b8
    NVNcommandBuffer tiny = {0}, other = {0};
    h_nvnCommandBufferInitialize(&tiny, NULL); h_nvnCommandBufferInitialize(&other, NULL);
    const size_t grants[] = {16, 40, 56};
    for (size_t k = 0; k < sizeof grants / sizeof grants[0]; k++) {
        size_t n = grants[k];
        h_nvnCommandBufferAddCommandMemory(&tiny, P, 0, n);
        EXPECT("a small grant records into the game's memory", (uintptr_t)m, (uintptr_t)CB(&tiny)->cur);
        EXPECT("its size is what the game added", n, h_nvnCommandBufferGetCommandMemorySize(&tiny));
        h_nvnCommandBufferBeginRecording(&tiny); h_nvnCommandBufferEndRecording(&tiny);
        EXPECT("an empty recording's END takes 8 bytes of it", 8, h_nvnCommandBufferGetCommandMemoryUsed(&tiny));
        EXPECT("and leaves the rest free", n - 8, h_nvnCommandBufferGetCommandMemoryFree(&tiny));
    }
    for (size_t k = 1; k < sizeof grants / sizeof grants[0]; k++) for (int lead = 0; lead < 2; lead++) {
        size_t n = grants[k];
        h_nvnCommandBufferAddCommandMemory(&tiny, P, 0, n);
        NVNcommandHandle h = rec_list(&tiny, lead, 0, 0x5);
        int s = seg_of((uintptr_t)m);
        EXPECT("a small grant's overflow keeps its guest origin", 1, s >= 0 && g_hostseg[s].org_hi > (uintptr_t)m && g_hostseg[s].org_hi <= (uintptr_t)(m + n));
        h_nvnCommandBufferAddCommandMemory(&tiny, P, 0, n);
        EXPECT("a regrant alone keeps it live", 1, s >= 0 && g_hostseg[s].busy);
        h_nvnCommandBufferAddCommandMemory(&other, P, 4096, n); rec_list(&other, 0, 0, 0xdead);
        EXPECT("the list survives another buffer's overflow", NB, list_ok(h, 0x5));
        h_nvnCommandBufferBeginRecording(&tiny); rec_one(&tiny, 0x6, 0, 16); h_nvnCommandBufferEndRecording(&tiny);
        EXPECT("writing over the small grant reclaims its overflow", 0, s >= 0 ? g_hostseg[s].busy : -1);
        int nseg = g_nhostseg, good = 1;
        for (int frame = 0; frame < 16; frame++) {
            h_nvnCommandBufferAddCommandMemory(&tiny, P, 0, n);
            h = rec_list(&tiny, lead, 0, 0x200u + (uint32_t)frame);
            good &= list_ok(h, 0x200u + (uint32_t)frame) == NB;
        }
        EXPECT("reused small grants replay each recording whole", 1, good);
        EXPECT("reused small grants need no new host segments", nseg, g_nhostseg);
        h_nvnCommandBufferAddCommandMemory(&tiny, P, 0, n); h_nvnCommandBufferBeginRecording(&tiny); h_nvnCommandBufferEndRecording(&tiny);
        h_nvnCommandBufferAddCommandMemory(&other, P, 4096, n); h_nvnCommandBufferBeginRecording(&other); h_nvnCommandBufferEndRecording(&other);
    }
    // A grant that cannot hold the 16-byte JUMP still goes to a host segment: 8 bytes, or 16 that start 4 bytes off 8-byte alignment
    h_nvnCommandBufferAddCommandMemory(&tiny, P, 0, 8);
    EXPECT("8 bytes: a host segment", 1, CB(&tiny)->cur && (CB(&tiny)->cur < m || CB(&tiny)->cur >= m + sizeof m));
    EXPECT("8 bytes: its size is still what the game added", 8, h_nvnCommandBufferGetCommandMemorySize(&tiny));
    h_nvnCommandBufferAddCommandMemory(&tiny, P, 4, 16);
    EXPECT("16 unaligned bytes: a host segment", 1, CB(&tiny)->cur && (CB(&tiny)->cur < m || CB(&tiny)->cur >= m + sizeof m));
    h_nvnCommandBufferAddCommandMemory(&tiny, P, 4, 20);
    EXPECT("20 bytes 4 off alignment hold a JUMP: the game's memory", (uintptr_t)(m + 8), (uintptr_t)CB(&tiny)->cur);
}

static void pool_goes(void) {
    // A list overflowed out of a pool that the game then finalizes (it moved its command memory to another pool): nothing writes there again, the pool going frees its segment
    static _Alignas(4096) uint8_t gone_mem[4096], live_mem[4096];
    pool_t gone = { .magic = MAGIC('P', 'O', 'O', 'L'), .mem = gone_mem, .size = sizeof gone_mem }, live = { .magic = MAGIC('P', 'O', 'O', 'L'), .mem = live_mem, .size = sizeof live_mem };
    pool_t virt = { .magic = MAGIC('P', 'O', 'O', 'L'), .flags = 0x200, .mem = live_mem, .size = sizeof live_mem };   // a virtual pool that MapVirtual pointed at live's memory
    NVNcommandBuffer ca = {0}, cb = {0}, cx = {0};
    h_nvnCommandBufferInitialize(&ca, NULL); h_nvnCommandBufferInitialize(&cb, NULL); h_nvnCommandBufferInitialize(&cx, NULL);
    h_nvnCommandBufferAddCommandMemory(&ca, (NVNmemoryPool *)&gone, 512, 64); rec_list(&ca, 0, 0, 0xa);
    h_nvnCommandBufferAddCommandMemory(&cb, (NVNmemoryPool *)&live, 512, 64); NVNcommandHandle hb = rec_list(&cb, 0, 0, 0xb);
    int a = seg_of((uintptr_t)(gone_mem + 512)), b = seg_of((uintptr_t)(live_mem + 512));
    EXPECT("a list in the pool that goes holds a segment", 1, a >= 0);
    EXPECT("a list in a pool that stays holds a segment", 1, b >= 0);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&virt);
    EXPECT("a virtual pool going frees nothing in the physical pool's memory", 1, b >= 0 && g_hostseg[b].busy);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&gone);
    EXPECT("the pool going frees the segment of its list", 0, a >= 0 ? g_hostseg[a].busy : -1);
    EXPECT("and keeps the segment of a list in another pool", 1, b >= 0 && g_hostseg[b].busy);
    EXPECT("whose list replays whole", NB, list_ok(hb, 0xb));
    int nseg = g_nhostseg;
    h_nvnCommandBufferAddCommandMemory(&ca, (NVNmemoryPool *)&live, 2048, 64); NVNcommandHandle hc = rec_list(&ca, 0, 0, 0xc);
    EXPECT("the next overflow takes the freed segment: no new one", nseg, g_nhostseg);
    EXPECT("and it does not disturb the live list", NB, list_ok(hb, 0xb));
    EXPECT("the new list replays whole", NB, list_ok(hc, 0xc));
    // retire this case's lists
    h_nvnCommandBufferAddCommandMemory(&cx, (NVNmemoryPool *)&live, 512, 64); h_nvnCommandBufferBeginRecording(&cx); h_nvnCommandBufferEndRecording(&cx);
    h_nvnCommandBufferAddCommandMemory(&cx, (NVNmemoryPool *)&live, 2048, 64); h_nvnCommandBufferBeginRecording(&cx); h_nvnCommandBufferEndRecording(&cx);
}

int main(void) {
    grant_is_no_write(); printf("grant is no write: %d busy of %d\n", busy_n(), g_nhostseg);
    record_that_does_not_fit(); printf("record that does not fit: %d busy of %d\n", busy_n(), g_nhostseg);
    small_grants(); printf("small grants: %d busy of %d\n", busy_n(), g_nhostseg);
    pool_goes(); printf("pool goes: %d busy of %d\n", busy_n(), g_nhostseg);
    if (failed) { printf("%d checks failed\n", failed); return 1; }
    puts("host-lent segment lifetime: PASS"); return 0;
}
'''


@unittest.skipUnless(posix(), 'a POSIX host C compiler is required')
class HostSegmentLifetime(unittest.TestCase):
    def test_segments_live_as_long_as_their_list(self):
        prefix = harness().split('static unsigned busy(void)')[0]
        with tempfile.TemporaryDirectory(prefix='nx2a-hostseg-lifetime-') as tmp:
            c, exe = Path(tmp)/'check.c', Path(tmp)/'check'
            c.write_text(prefix + finalize() + CHECKS, encoding='utf-8')
            # -fno-strict-aliasing as in the Android build (nxnative/dm/android/build.py): hle_nvn.c writes its cmd_t over the opaque NVNcommandBuffer
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-fno-strict-aliasing', '-Wall', '-Wextra', '-Wno-unused-variable',
                            '-Wno-unused-function', '-Wno-unused-parameter', '-I', str(NVN), '-I', str(NVN/'include'), str(c),
                            '-pthread', '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
