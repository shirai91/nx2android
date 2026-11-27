# Fate/Samurai Remnant (01003AE01AA76000): the rewriter's patches of this game's own builds. dm/tools/patch_nso.py loads every profile's patches.py and applies those of a module's
# NSO build ID; CUTS, OBS and FRAME_STEP as described there ("Registered patches"). Only a render-scale stub: no frame step (the game keeps its own interval logic, state target 0)
from nso_asm import ADRP, B

# Render scale (state +0x10, float written by the loader: DM_RENDER_SCALE, 1.0 by default; DM_DYN_SCALE steers it from the GPU time). The KTGL renderer keeps a render resolution
# ratio at +0x1800 (1.0 at init, main+0x7fd30; reflected as "RenderResolutionRatio"): the scene is drawn into ratio x the render-target size and the passes after it read the ratio
# (1 / ratio as a UV scale at main+0x81fd0 and main+0x83664, ratio x size / 8 as compute groups at main+0x8830c). main+0x3fd47c stores the scene parameters' ratio into it; the stub
# multiplies the stored value by the host's factor. s16 is unused in that function and x8 is reloaded right after (main+0x3fd490), so both are free at the site
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    fs_site(0x3fd47c, '600218bd')
    start = here(); emit(ADRP(8, here(), FS_STATE_OFF))
    for w in (0xbd401110,   # ldr s16,[x8,#0x10]   render scale
              0x1e202208,   # fcmp s16,#0.0
              0x5400004d,   # b.le skip (+8)       not set
              0x1e300800,   # fmul s0,s0,s16
              0xbd180260):  # skip: str s0,[x19,#0x1800] (original)
        emit(w)
    emit(B(here(), 0x3fd480))
    fs_put(0x3fd47c, [B(0x3fd47c, start)], 'render resolution ratio x the host render scale (1.0 = original)'); ctx.render_scale = 1

FRAME_STEP = {'6c46135714abb8706eee10f330f12b97': frame_step}
