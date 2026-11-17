"""The movie decoder's timestamps: the game's own time, and the made-up one where the game's does not move.

The Android path queues each packet with a timestamp of its own (the game's while the times increase, else the last one + 1 µs) and maps that timestamp back to the game's own
time on the way out (pmap). A game that sends every packet with time 0 (13 Sentinels: Aegis Rim sends its 1024x576 movies that way, and Catherine: Full Body's player advances its
clock with the picture's time, so it stops feeding the decoder when the picture it shows does not move) needs a time that moves: there the made-up time is used. A stream whose
times do move keeps the game's own times: a stream with B-frames sends its packets in decode order, so its times go down at each B-frame, and they must come back out as the game
sent them.

The production functions are cut out of dm/android/movie_avc.c and run here.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class AVCPictureTime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / 'nxnative/dm/android/movie_avc.c').read_text(encoding='utf-8')
        at = source.index('static int64_t mavc_codec_time(')
        last = source.index('static int64_t mavc_map_time(')
        production = source[at:source.index('\n}', last) + 2]
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct { int64_t last_in, last_game; int synth; struct { int64_t c, g; } pmap[64]; int npmap; } mavc;
''' + production + r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    int mode = atoi(argv[1]);
    mavc a = {0}; a.last_in = a.last_game = INT64_MIN;
    int64_t cpts[8], out[8]; int n = 0;
    switch (mode) {
    case 0: {   // the game's times increase: the pictures keep them
        int64_t p[] = {0, 100000, 200000};
        for (int i = 0; i < 3; i++) { cpts[n] = mavc_map_time(&a, p[i]); out[n] = mavc_picture_time(&a, cpts[n]); n++; }
        assert(cpts[0] == 0 && cpts[1] == 100000 && cpts[2] == 200000);
        assert(out[0] == 0 && out[1] == 100000 && out[2] == 200000);
        assert(!a.synth);   // the times the game sent are the codec's own
        break; }
    case 1: {   // every packet with time 0 (13 Sentinels: Aegis Rim): the pictures' times move
        for (int i = 0; i < 4; i++) { cpts[n] = mavc_map_time(&a, 0); out[n] = mavc_picture_time(&a, cpts[n]); n++; }
        assert(cpts[0] == 0 && cpts[1] == 1 && cpts[2] == 2 && cpts[3] == 3);
        assert(out[0] == 0 && out[1] == 1 && out[2] == 2 && out[3] == 3);
        assert(a.synth);   // the made-up timestamps: the codec's own output order stands
        break; }
    case 2: {   // a stream with B-frames: its times go down at each B-frame; the pictures keep the game's own times
        int64_t p[] = {0, 100000, 33000, 66000};
        for (int i = 0; i < 4; i++) { cpts[n] = mavc_map_time(&a, p[i]); out[n] = mavc_picture_time(&a, cpts[n]); n++; }
        assert(cpts[0] == 0 && cpts[1] == 100000 && cpts[2] == 100001 && cpts[3] == 100002);
        assert(out[0] == 0 && out[1] == 100000 && out[2] == 33000 && out[3] == 66000);
        break; }
    case 3: {   // a time that repeats after one that moved: the made-up one again
        int64_t p[] = {0, 100000, 100000};
        for (int i = 0; i < 3; i++) { cpts[n] = mavc_map_time(&a, p[i]); out[n] = mavc_picture_time(&a, cpts[n]); n++; }
        assert(out[0] == 0 && out[1] == 100000 && out[2] == 100001);
        break; }
    case 4: {   // a fresh stream: the first packet keeps the game's own time, even at 0
        mavc_map_time(&a, 0);
        assert(a.last_game == 0);
        break; }
    }
    return 0;
}
'''
        cls.temp = tempfile.TemporaryDirectory(prefix='nx2a-avc-time-'); cls.addClassCleanup(cls.temp.cleanup)
        c = Path(cls.temp.name) / 'test.c'; cls.binary = c.with_suffix('.exe' if os.name == 'nt' else ''); c.write_text(code, encoding='utf-8')
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Werror', str(c), '-o', str(cls.binary)], check=True, capture_output=True, text=True)

    def test_the_game_times_and_the_made_up_ones(self):
        for case in range(5):
            with self.subTest(case=case):
                r = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
