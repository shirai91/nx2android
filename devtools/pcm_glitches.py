"""pcm_glitches.py DUMP [--block 240]: find audible faults in a DM_AUDREN_DUMP recording (stereo int16, 48 kHz, the blocks in play order).

Reports, with times:
- clipping: samples at full scale (the host's 6-to-2 channel downmix clamps to int16);
- gaps: all-zero blocks between non-silent blocks (a render that produced nothing, zero-filled by the host);
- clicks: sample steps far above the local signal's own steps (a discontinuity), and how many of them fall on a block edge
  (a frame boundary: frames out of order, a repeated or dropped frame);
- level: the loudest second.
Needs numpy. Run with plain python (user site-packages)."""
import sys, argparse
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument('dump'); ap.add_argument('--block', type=int, default=240); ap.add_argument('--rate', type=int, default=48000)
ap.add_argument('--show', type=int, default=20, help='list this many of each kind')
a = ap.parse_args()
x = np.fromfile(a.dump, dtype='<i2')
x = x[: len(x) // 2 * 2].reshape(-1, 2).astype(np.int32)
n = len(x); B = a.block; R = a.rate
print(f'{n} frames, {n / R:.1f} s, peak {np.abs(x).max()}')
t = lambda i: f'{i / R:8.3f} s'

# clipping
clip = np.where((x >= 32767) | (x <= -32768))[0]
runs = np.split(clip, np.where(np.diff(clip) > 1)[0] + 1) if len(clip) else []
print(f'clipping: {len(clip)} samples at full scale in {len(runs)} runs')
for r in runs[: a.show]: print(f'  {t(r[0])} {len(r)} samples')

# gaps: zero blocks between sound
nb = n // B
blk = np.abs(x[: nb * B]).reshape(nb, B, 2).max(axis=(1, 2))
loud = blk > 64
zero = blk == 0
gaps = [i for i in range(1, nb - 1) if zero[i] and loud[i - 1:i + 3].any() and loud[max(0, i - 4):i].any()]
print(f'gaps: {len(gaps)} all-zero blocks next to sound')
for i in gaps[: a.show]: print(f'  {t(i * B)} block {i}')

# repeats: blocks that are exact copies of an earlier block (a voice that played its looping stream buffer again before the game refilled it)
blocks = x[: nb * B].reshape(nb, B * 2)
for lag in range(1, 9):
    rep = (blocks[lag:] == blocks[:-lag]).all(axis=1) & loud[lag:]
    if rep.sum(): print(f'repeats: {rep.sum()} sounding blocks are copies of the block {lag} earlier ({100 * rep.sum() / max(1, loud.sum()):.1f}% of the sounding blocks)')
print('repeats: checked lags 1-8')

# clicks: first difference far above the local median step (20 ms window), on the louder channel
d = np.abs(np.diff(x, axis=0)).max(axis=1).astype(np.float64)
w = 960
med = np.convolve(d, np.ones(w) / w, mode='same') + 8
ratio = d / med
cand = np.where((ratio > 12) & (d > 2000))[0] + 1
edge = cand[(cand % B) == 0]
print(f'clicks: {len(cand)} steps above 12x the local mean step; {len(edge)} of them on a {B}-sample block edge (expected by chance {len(cand) / B:.1f})')
for i in cand[: a.show]: print(f'  {t(i)} step {int(d[i - 1])} ({ratio[i - 1]:.0f}x) {"EDGE" if i % B == 0 else ""}')

# loudest second
sec = n // R
if sec:
    rms = np.sqrt((x[: sec * R].astype(np.float64) ** 2).reshape(sec, R, 2).mean(axis=(1, 2)))
    print(f'level: loudest second {int(rms.argmax())} s rms {rms.max():.0f}; median second rms {np.median(rms):.0f}')
