"""The phone d-pad keeps all eight sliding directions and a neutral center."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class DualPadTest(unittest.TestCase):
    def test_eight_directions_and_center(self):
        with tempfile.TemporaryDirectory() as t:
            p = pathlib.Path(t) / 'org/nx2a/Check.java'
            p.parent.mkdir(parents=True)
            p.write_text('''package org.nx2a;
public class Check {
 public static void main(String[] args) {
  long r=1L<<14,d=1L<<15,l=1L<<12,u=1L<<13;
  float[][] v={{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1},{0,0},{.1f,.1f}};
  long[] want={r,r|d,d,d|l,l,l|u,u,u|r,0,0};
  for(int i=0;i<want.length;i++) if(PadMath.direction(v[i][0],v[i][1])!=want[i])throw new AssertionError(i);
 }
}''')
            subprocess.run(['javac', '-d', t, str(ROOT / 'nxnative/dm/android/java/org/nx2a/PadMath.java'), str(p)], check=True)
            subprocess.run(['java', '-cp', t, 'org.nx2a.Check'], check=True)
