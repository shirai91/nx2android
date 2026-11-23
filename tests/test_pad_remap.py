"""Unit tests for Android controller button remapping & raw scancode support (pad_remap.h)."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'


class PadRemapTest(unittest.TestCase):
    def test_remap_secondary_held_and_persistence(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the pad remap check')
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pad_remap.h"

int main(int argc, char **argv) {
    assert(argc == 2);
    pad_remap_table t = {0};
    char name[32];

    // 1. Default Position vs Label face button mapping
    assert(pad_remap_eval(&t, 0, PAD_KC_BTN_A) == DM_BTN_B);
    assert(pad_remap_eval(&t, 0, PAD_KC_BTN_B) == DM_BTN_A);
    assert(pad_remap_eval(&t, 1, PAD_KC_BTN_A) == DM_BTN_A);
    assert(pad_remap_eval(&t, 1, PAD_KC_BTN_B) == DM_BTN_B);
    assert(pad_remap_eval(&t, 0, PAD_KC_BTN_L1) == DM_BTN_L);
    assert(pad_remap_eval(&t, 0, PAD_KC_BTN_L2) == DM_BTN_ZL);

    // 2. System, phone and media keys are blocked; raw scancodes work when keycode == 0
    assert(pad_phys_from_key(PAD_KC_BACK, 158) == 0);
    assert(pad_phys_from_key(PAD_KC_HOME, 102) == 0);
    assert(pad_phys_from_key(PAD_KC_VOLUME_UP, 115) == 0);
    assert(pad_phys_from_key(PAD_KC_CALL, 0) == 0);
    assert(pad_phys_from_key(PAD_KC_HEADSETHOOK, 0) == 0);
    assert(pad_phys_from_key(PAD_KC_MEDIA_PLAY_PAUSE, 0) == 0);
    assert(pad_phys_from_key(PAD_KC_ASSIST, 0) == 0);
    assert(pad_phys_from_key(PAD_KC_BRIGHTNESS_UP, 0) == 0);
    assert(pad_phys_from_key(PAD_KC_BTN_1, 314) == PAD_KC_BTN_1);
    uint32_t extra_scan = pad_phys_from_key(0, 704);
    uint32_t extra_key  = pad_phys_from_key(PAD_KC_BTN_2, 705);
    assert(PAD_PHYS_IS_SCAN(extra_scan) && PAD_PHYS_VAL(extra_scan) == 704);
    assert(!PAD_PHYS_IS_SCAN(extra_key) && extra_key == PAD_KC_BTN_2);

    // 3. Bind raw scancode as Secondary for Switch B while keeping Primary BTN A intact
    pad_remap_set(&t, 0, 1, 1, extra_scan);   // bit 1 = DM_BTN_B, slot 1 = Secondary
    pad_remap_set(&t, 0, 2, 1, extra_key);    // bit 2 = DM_BTN_X, slot 1 = Secondary
    assert(t.custom == 1);
    assert(pad_remap_eval(&t, 0, PAD_KC_BTN_A) == DM_BTN_B);
    assert(pad_remap_eval(&t, 0, extra_scan) == DM_BTN_B);
    assert(pad_remap_eval(&t, 0, PAD_KC_BTN_Y) == DM_BTN_X);
    assert(pad_remap_eval(&t, 0, extra_key) == DM_BTN_X);

    // 4. Many-to-one held tracking: holding both Primary and Secondary, releasing one keeps bit held
    pad_held_keys h = {0};
    pad_held_update(&h, PAD_KC_BTN_A, 1, 0);
    pad_held_update(&h, extra_scan, 1, 0);
    assert(pad_held_eval(&h, &t, 0) == DM_BTN_B);
    pad_held_update(&h, PAD_KC_BTN_A, 0, 1);
    assert(pad_held_eval(&h, &t, 0) == DM_BTN_B);
    pad_held_update(&h, extra_scan, 0, 1);
    assert(pad_held_eval(&h, &t, 0) == 0);

    // 5. Rebinding an already-used input moves it cleanly without leaving duplicates
    pad_remap_set(&t, 0, 0, 1, extra_scan);   // move from B to A
    assert(pad_remap_eval(&t, 0, extra_scan) == DM_BTN_A);
    assert(pad_remap_slot(&t, 0, 1, 1) == 0);

    // 6. Remapping analog trigger L2 (axes[4]) redirects cleanly without phantom ZL fire
    pad_remap_set(&t, 0, 1, 0, PAD_KC_BTN_L2);
    float axes[8] = {0, 0, 0, 0, 0.9f, 0, 0, 0};
    assert(pad_remap_axes_eval(axes, &t, 0) == DM_BTN_B);

    // 7. Flipping Face buttons (Position <-> Label) swaps A/B/X/Y while preserving Secondary binds
    pad_remap_table f = {0};
    pad_remap_set(&f, 0, 1, 1, extra_scan);
    assert(pad_remap_slot(&f, 0, 0, 0) == PAD_KC_BTN_B);
    assert(pad_remap_slot(&f, 0, 1, 0) == PAD_KC_BTN_A);
    pad_remap_flip_face_labels(&f);
    assert(pad_remap_slot(&f, 1, 0, 0) == PAD_KC_BTN_A);
    assert(pad_remap_slot(&f, 1, 1, 0) == PAD_KC_BTN_B);
    assert(pad_remap_slot(&f, 1, 1, 1) == extra_scan);

    // 8. Summary & physical names formatting
    pad_remap_summary(&f, 1, 1, name, sizeof name);
    assert(!strcmp(name, "BTN B + Scan 704"));
    pad_phys_name(extra_key, name, sizeof name);
    assert(!strcmp(name, "BTN 2"));

    // 9. Clear slot & settings.txt save/load roundtrip
    pad_remap_clear(&t, 0, 8, 0);   // ZL primary explicitly cleared
    FILE *fp = fopen(argv[1], "w+b"); assert(fp);
    pad_remap_write(&t, fp);
    long n = ftell(fp); rewind(fp);
    char *buf = calloc((size_t)n + 1, 1);
    assert(buf && fread(buf, 1, (size_t)n, fp) == (size_t)n);
    fclose(fp);
    pad_remap_table loaded = {0};
    for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) {
        char line[128]; snprintf(line, sizeof line, "%s\n", l);
        assert(pad_remap_parse(&loaded, line));
    }
    free(buf);
    assert(loaded.custom == 1);
    for (int b = 0; b < PAD_BTN_N; b++)
        for (int s = 0; s < PAD_SLOTS_N; s++)
            assert(loaded.binds[b][s] == t.binds[b][s]);

    // 10. Reset defaults clears custom flag
    pad_remap_reset(&loaded, 0);
    assert(loaded.custom == 0);
    assert(pad_remap_eval(&loaded, 0, PAD_KC_BTN_L2) == DM_BTN_ZL);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='nx2a_remap_') as tmp:
            tmp = Path(tmp)
            c, exe = tmp / 'check.c', tmp / 'check'
            c.write_text(harness, encoding='utf-8')
            result = subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', f'-I{DM / "android"}', f'-I{DM / "host"}',
                                     str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe), str(tmp / 'settings.txt')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
