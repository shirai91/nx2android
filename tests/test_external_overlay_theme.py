"""The external controller consumes the native overlay's real atlas manifest."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ExternalOverlayThemeTest(unittest.TestCase):
    def test_both_layouts_and_invalid_manifest(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            check = root / 'org/nx2a/Check.java'
            check.parent.mkdir(parents=True)
            check.write_text('''package org.nx2a;
import java.io.*;
public class Check {
 public static void main(String[] args) throws Exception {
  for (String layout : new String[]{"phone", "tablet"}) {
   OverlayTheme t = OverlayTheme.read(new FileReader(args[0]), layout);
   if (!t.image.equals(layout + ".png") || t.scale <= 0 || t.sprites.isEmpty()) throw new AssertionError(layout);
   boolean up=false, down=false, left=false, right=false;
   for (OverlayTheme.Sprite s : t.sprites) if (s.control.equals("dpad") && s.state.equals("idle")) {
    up |= s.direction.equals("up"); down |= s.direction.equals("down");
    left |= s.direction.equals("left"); right |= s.direction.equals("right");
   }
   if (!(up && down && left && right)) throw new AssertionError("missing dpad sprite");
  }
  try { OverlayTheme.read(new StringReader("layout\\tphone\\t1\\tbroken\\ta.png\\t2\\t1"), "phone"); throw new AssertionError(); }
  catch (IOException expected) {}
 }
}''')
            source = ROOT / 'nxnative/dm/android/java/org/nx2a/OverlayTheme.java'
            manifest = ROOT / 'nxnative/dm/android/themes/ergo/theme.tsv'
            subprocess.run(['javac', '-d', t, str(source), str(check)], check=True)
            subprocess.run(['java', '-cp', t, 'org.nx2a.Check', str(manifest)], check=True)


if __name__ == '__main__':
    unittest.main()
