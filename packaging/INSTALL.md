# Installing nx2android

nx2android ports your own Nintendo Switch games to Android. This folder is the whole app: there is nothing to install with pip, and you need no programming. You need:

- a Windows PC with about 30 GB free (the tools take 4 GB, a port about twice the game's size);
- an Android device whose GPU is an **Adreno** (Snapdragon) or **Xclipse** (Exynos 2200 and later). Mali and PowerVR GPUs cannot run these games;
- the game's files dumped from your own Switch (the base game NSP or XCI, its update and DLC, in one folder) and your **`prod.keys`** from the same Switch. The tool reads NSP and XCI files, and NSZ and XCZ files, which it decompresses while it ports (they need that much more free space for a while).

## 1. Start the app

Unzip `nx2android-<version>.zip` anywhere (a short path such as `C:\nx2android` is best) and start **nx2android.exe**. Windows may warn about an unknown app: choose *More info* → *Run anyway*.

A few antivirus engines may flag `nx2android.exe` (on VirusTotal, 3 of 69 flagged v0.1.1, with generic labels such as "Trojan.Stealer" or "BehavesLike.Injector"; Microsoft Defender, Kaspersky, ESET, BitDefender and the other large engines did not). It is a false positive: the app is a Python program packed with PyInstaller, whose small launcher unpacks a compressed archive of the program and runs it, which heuristic scanners mistake for a packed trojan. The app is not code-signed yet, which adds to the score. Its source is on the project's GitHub page, and you can build the same bundle yourself with `python packaging/build_bundle.py`.

## 2. Setup tab (once)

1. Click **Download missing tools**. The app downloads the Android NDK, the .NET SDK and Android platform-tools from Google and Microsoft (about 1 GB) into your nx2android folder. They come under their makers' licenses, which the dialog links to. If you already have them installed, set their paths instead.
2. Set **prod.keys** to your keys file and click **Save and check**.
3. Every line should now have a green ✓.

## 3. Port tab

1. Choose the game's folder. The card under it says which game it found and whether it can be ported: the **Games** tab lists the supported ones.
2. Click **Port**. The first port of a game takes a while: extraction, rewriting the game's code, translating its shaders (2 to 6 minutes on a fast 20-thread PC with an SSD, much longer on a slow one or a hard disk). Later ports reuse what has not changed.
3. The APK and the game's data folder (`<Game>-data`) appear in the game's folder. **Open output folder** shows them.

## 4. Put it on the device

With **USB debugging** on (Settings → About → tap *Build number* seven times, then Settings → Developer options → USB debugging) and the device plugged in, tick **Install on the device when done** before porting, or click **Install on device** afterwards. Allow the PC when the device asks.

## When something goes wrong

The log at the bottom of the Port tab says why; lines in red are errors. Ask on the project's GitHub page (Issues), with the game's title ID, your device and the red lines. Never post your keys or game files.

The command line is the same program: `nx2android.exe -m porter -h`.
