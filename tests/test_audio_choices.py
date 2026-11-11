import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'nxnative/dm/android/java/org/nx2a/AudioChoices.java'

class AudioChoicesTest(unittest.TestCase):
    def test_call_only_device_is_hidden_and_media_keeps_its_name(self):
        stub = '''package android.media; public class AudioDeviceInfo {
 public static final int TYPE_BUILTIN_SPEAKER=2, TYPE_BLUETOOTH_SCO=7, TYPE_BLUETOOTH_A2DP=8, TYPE_HDMI=9, TYPE_HDMI_ARC=10, TYPE_USB_DEVICE=11, TYPE_USB_HEADSET=22, TYPE_BLE_HEADSET=26, TYPE_BLE_SPEAKER=27;
 private final int type; private final String name; public AudioDeviceInfo(int t,String n){type=t;name=n;}
 public int getType(){return type;} public CharSequence getProductName(){return name;}
}'''
        check = '''package org.nx2a; import android.media.AudioDeviceInfo; public class Check {
 public static void main(String[] args) {
  if(AudioChoices.name(new AudioDeviceInfo(7,"FreeClip")) != null) throw new AssertionError("call device");
  if(!"HUAWEI FreeClip".equals(AudioChoices.name(new AudioDeviceInfo(8,"HUAWEI FreeClip")))) throw new AssertionError("media name");
  if(!"HUAWEI FreeClip".equals(AudioChoices.name(new AudioDeviceInfo(26,"HUAWEI FreeClip")))) throw new AssertionError("BLE name");
  if(!"Bluetooth audio".equals(AudioChoices.name(new AudioDeviceInfo(8," ")))) throw new AssertionError("empty name");
  if(!"Phone speaker".equals(AudioChoices.name(new AudioDeviceInfo(2,"speaker")))) throw new AssertionError("speaker");
  if(!"External audio · Monitor".equals(AudioChoices.name(new AudioDeviceInfo(9,"Monitor")))) throw new AssertionError("HDMI");
 }}'''
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            audio = root / 'android/media/AudioDeviceInfo.java'; audio.parent.mkdir(parents=True); audio.write_text(stub)
            test = root / 'org/nx2a/Check.java'; test.parent.mkdir(parents=True); test.write_text(check)
            subprocess.run(['javac', '-d', tmp, str(audio), str(SOURCE), str(test)], check=True)
            subprocess.run(['java', '-cp', tmp, 'org.nx2a.Check'], check=True)

if __name__ == '__main__': unittest.main()
