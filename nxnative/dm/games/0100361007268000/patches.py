# MotoGP 18 (0100361007268000): widescreen 1280x720 -> 1600x720. The game hardcodes
# its display size in a BSS pair (W at main+0x64d43b8, H at main+0x64d43c0) and its
# render-size reference in an aspect-manager struct (+0xac/+0xb4); it never calls
# nn::oe::GetDefaultDisplayResolution, so the host HLE override cannot affect it.
# Round 1 (W1/W2/W3/R1) widened the display size; the swap chain went 1600x720 but
# the game still pillarboxed all content to a centered 1280 viewport. Round 2
# (P1/P2, REMOVED) patched the 16:9 pillarbox width function 0x146f6b4 — it FAILED
# in-game with 6/6 cuts verified (2026-11-23 08:08), so that function is not the
# viewport source. Round 3 (V3, REMOVED) forced 1600x720 into the render-size
# struct via the metrics funnel — it FAILED in-game (80.0% unchanged, 2026-11-23
# 09:14), and round 7 proved its theory wrong (slot 51 is a no-op; no 1280x720
# metrics fill exists in main). Rounds 5-6 mapped a scale-factor path in
# 0x1ced80c, but the runtime hook (2026-11-23 12:03, 8 calls, ALL gate-false)
# proved that path NEVER runs — the inline (1,1) patch is VOID. Round 7 also
# proved the 0x1ced80c else branch yields only a 3% safe-area inset (48,22)/(0,0),
# never (160,0). The remaining off-center carrier is the CACHED projection matrix
# in 0xe6e6ac ([x19+0x850], used when flag byte [x19+0x844]!=0); the compute path
# is symmetric in both branches and cannot pillarbox. C1 (DIAGNOSTIC, 1 word)
# forces the compute path: if the pillarbox is gone, the cached matrix was the
# source; if nothing changes, the source is post-projection/shader/subsdk0.
# See ~/workspace/motogp18-widescreen/FINDINGS.md (round 1),
# ~/workspace/motogp18-widescreen/FINDINGS_VIEWPORT.md (round 2, dead end),
# ~/workspace/motogp18-widescreen/FINDINGS_VIEWPORT2.md (round 3) and
# ~/workspace/motogp18-widescreen/FINDINGS_CALCEXTENTS.md sections 10-11
# (hook results + round 7).
CUTS = {
  # MotoGP 18 v0 main
  '758a0602b2c99b4395277296bc002848': [
    # W1: standalone display-size setter 0x294c794: *W = 1280 -> 1600
    (0x294c7a4, '0aa08052', '0ac88052', 'widescreen: display width setter mov w10,#0x500 -> mov w10,#0x640'),
    # W2: .init_array writer 0x2978398 (runs at startup): *W = 1280 -> 1600
    (0x2978d80, '09a08052', '09c88052', 'widescreen: init_array display width mov w9,#0x500 -> mov w9,#0x640'),
    # W3: arg-parser default 0x3f9490 (-ResX= default): *W = 1280 -> 1600
    (0x3f9510, '09a08052', '09c88052', 'widescreen: -ResX default mov w9,#0x500 -> mov w9,#0x640'),
    # R1: render-size/aspect-reference init 0x17e8718 (vtable slot 81): +0xac = 1280 -> 1600
    (0x17e8750, '09a08052', '09c88052', 'widescreen: aspect reference width mov w9,#0x500 -> mov w9,#0x640'),
    # C1 (DIAGNOSTIC): projection 0xe6e6ac uses a CACHED matrix from [x19+0x850]
    # when flag byte [x19+0x844]!=0 (cbz w8,#0xe6e964 at 0xe6e8f0 falls through
    # to the cached copy); the compute path is symmetric and cannot pillarbox.
    # Zero the flag test so the compute path always runs. w8 is untouched
    # between the ldrb and the cbz, and the compute path reloads w8 immediately
    # (ldrb w8,[x19,#0x318] at 0xe6e968), so this is register-safe.
    (0xe6e8dc, '68126139', 'e8031f2a', 'viewport: force compute path in projection 0xe6e6ac (ldrb w8,[x19,#0x844] -> mov w8,wzr)'),
  ],
}
