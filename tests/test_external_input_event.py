"""Moves must not overwrite a controller button or another input stream."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ExternalInputEventTest(unittest.TestCase):
    def test_coalescing_preserves_keys_and_devices(self):
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('a C compiler is needed')
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            header = root / 'android/input.h'; header.parent.mkdir()
            header.write_text('''
typedef struct { int type, source, device, action; } AInputEvent;
enum { AINPUT_EVENT_TYPE_KEY=1, AINPUT_EVENT_TYPE_MOTION=2, AMOTION_EVENT_ACTION_MASK=255, AMOTION_EVENT_ACTION_MOVE=2 };
static inline int AInputEvent_getType(const AInputEvent *e) { return e->type; }
static inline int AInputEvent_getSource(const AInputEvent *e) { return e->source; }
static inline int AInputEvent_getDeviceId(const AInputEvent *e) { return e->device; }
static inline int AMotionEvent_getAction(const AInputEvent *e) { return e->action; }
''')
            source = root / 'check.c'
            source.write_text('''
#include <assert.h>
#include "external_input_event.h"
int main(void) {
 AInputEvent a={2,0x1000010,1,2}, b=a, key={1,0x401,1,2};
 assert(external_moves_match(&a,&b));
 assert(!external_moves_match(&a,&key)); assert(!external_moves_match(&key,&a));
 b.device=2; assert(!external_moves_match(&a,&b)); b=a;
 b.source=0x1000011; assert(!external_moves_match(&a,&b)); b=a;
 b.action=1; assert(!external_moves_match(&a,&b));
 return 0;
}
''')
            exe = root / 'check'
            subprocess.run([cc, '-Wall', '-Wextra', '-Werror', '-I'+t, '-I'+str(ROOT/'nxnative/dm/android'), str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
