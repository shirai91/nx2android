<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/logo-dark.svg">
    <img alt="nx2android" src="docs/images/logo.svg" height="140">
  </picture>
</h1>

Port your own Nintendo Switch games to Android, natively: the game's own arm64 code runs on the device, with the Switch's system services stood in for by a host and NVN graphics replayed on Vulkan. Pick the game's folder, click **Port**, and get an APK plus the game's data, installed on your device if you like. Bring your own game files and keys; nothing from any game is in this repository.

This is the Android port of [nxnative](https://github.com/seedds/nxnative), which does the same on iPhone and iPad. Supported games: [Games](#games).

## Games

These games play on Android. Tested on a Lenovo Legion Y700 (Snapdragon 8 Elite, Adreno 830). The device's GPU must read BC-compressed textures (Adreno, Xclipse; not Mali or PowerVR).

| | |
|:---:|:---:|
| ![13 Sentinels: Aegis Rim](docs/images/13-sentinels.jpg)<br>13 Sentinels: Aegis Rim<br><sub>`v0.1.3` · update 1.10 · 60 fps</sub> | ![Advance Wars 1+2: Re-Boot Camp](docs/images/advance-wars-1-2-re-boot-camp.jpg)<br>Advance Wars 1+2: Re-Boot Camp<br><sub>`v0.1.3` · base game 1.0.0 · 60 fps (30 in battle on Switch)</sub> |
| ![Blasphemous 2](docs/images/blasphemous-2.jpg)<br>Blasphemous 2<br><sub>`v0.1.3` · update 1.0.8 · 60 fps</sub> | ![Captain Toad: Treasure Tracker](docs/images/captain-toad-treasure-tracker.jpg)<br>Captain Toad: Treasure Tracker<br><sub>`v0.1.3` · update 1.4.2 · 60 fps</sub> |
| ![Castlevania: Belmont's Curse](docs/images/castlevania-belmonts-curse.jpg)<br>Castlevania: Belmont's Curse<br><sub>`v0.2.0` · update 1.1.0 · 60 fps</sub> | ![Celeste](docs/images/celeste.jpg)<br>Celeste<br><sub>`v0.1.3` · update 1.4.1.1 · 60 fps</sub> |
| ![Donkey Kong Country Returns HD](docs/images/donkey-kong-country-returns-hd.jpg)<br>Donkey Kong Country Returns HD<br><sub>`v0.1.3` · update 1.1.0 · 60 fps</sub> | ![Graveyard Keeper 2](docs/images/graveyard-keeper-2.jpg)<br>Graveyard Keeper 2<br><sub>`v0.1.2` · update 1.0.4 · 60 fps (30 on Switch)</sub> |
| ![Hades II](docs/images/hades-ii.jpg)<br>Hades II<br><sub>`v0.1.2` · update 1.0 · 60 fps</sub> | ![Hollow Knight: Silksong](docs/images/hollow-knight-silksong.jpg)<br>Hollow Knight: Silksong<br><sub>`v0.1.3` · update 1.0.30000 · 60 fps</sub> |
| ![Mario vs. Donkey Kong](docs/images/mario-vs-donkey-kong.jpg)<br>Mario vs. Donkey Kong<br><sub>`v0.1.3` · update 1.0.1 · 60 fps</sub> | ![Metroid Dread](docs/images/metroid-dread.jpg)<br>Metroid Dread<br><sub>`v0.2.0` · update 2.1.0 · 60 fps</sub> |
| ![NEO: The World Ends with You](docs/images/neo-the-world-ends-with-you.jpg)<br>NEO: The World Ends with You<br><sub>`v0.1.2` · update 1.03</sub> | ![Paper Mario: The Origami King](docs/images/paper-mario-the-origami-king.jpg)<br>Paper Mario: The Origami King<br><sub>`v0.2.0` · update 1.0.1 · 60 fps (30 on Switch)</sub> |
| ![Paper Mario: The Thousand-Year Door](docs/images/paper-mario-the-thousand-year-door.jpg)<br>Paper Mario: The Thousand-Year Door<br><sub>`v0.2.0` · update 1.0.1 · 60 fps (30 on Switch) · **work in progress**: playable, but lighting flickers (walls go black for single frames after a fall)</sub> | ![Pokémon Legends: Arceus](docs/images/pokemon-legends-arceus.jpg)<br>Pokémon Legends: Arceus<br><sub>`v0.2.0` · update 1.1.1 · 60 fps (30 on Switch)</sub> |
| ![Super Mario Bros. Wonder](docs/images/super-mario-bros-wonder.jpg)<br>Super Mario Bros. Wonder<br><sub>`v0.1.3` · update 1.2.1 · 60 fps, dips to 40</sub> | ![Super Mario Odyssey](docs/images/super-mario-odyssey.jpg)<br>Super Mario Odyssey<br><sub>`v0.2.0` · update 1.4.1 · 60 fps · shake the device to throw Cappy</sub> |
| ![Tales of Eternia Remastered](docs/images/tales-of-eternia-remastered.jpg)<br>Tales of Eternia Remastered<br><sub>`v0.1.2` · update 2.0.0 · 60 fps</sub> | ![Unicorn Overlord](docs/images/unicorn-overlord.jpg)<br>Unicorn Overlord<br><sub>`v0.1.3` · update 1.05 · 60 fps · **boots**: saving and a full battle not tested yet</sub> |
| ![Unpacking](docs/images/unpacking.jpg)<br>Unpacking<br><sub>`v0.1.2` · update 2.01 · 60 fps · played by touch</sub> | ![Viractal: Will You Trust Your Party?](docs/images/viractal.jpg)<br>Viractal: Will You Trust Your Party?<br><sub>`v0.1.0` · update 1.4.5</sub> |
| ![Xenoblade Chronicles 3](docs/images/xenoblade-chronicles-3.jpg)<br>Xenoblade Chronicles 3<br><sub>`v0.1.0` · update 2.2.0 · 30 fps · **work in progress**: some effects and models don't draw yet</sub> | |

The tag (`v0.1.3`) is the latest nx2android version the game was tested on. Older games are not tested again for each release: if a later version breaks one, a patch release fixes it. A tag newer than the [latest release](https://github.com/shirai91/nx2android/releases/latest) is the next release, and the game needs the development checkout until it ships.

A game that is not in this table is not supported yet. The games we look at next, and the ones done: [docs/PORT_CHECKLIST.md](docs/PORT_CHECKLIST.md).

## For players: port a game without programming

1. Download `nx2android-<version>-windows-x64.zip` (Windows) or `nx2android-<version>-macos-arm64.zip` (Mac with Apple silicon) from [Releases](https://github.com/shirai91/nx2android/releases/latest), unzip it and start **nx2android** (on a Mac, see [packaging/INSTALL.md](packaging/INSTALL.md#1-start-the-app) first).
2. **Setup** tab: click **Download missing tools** (the Android NDK, the .NET SDK and platform-tools, about 1 GB, from Google and Microsoft), then set your **prod.keys**. Every line turns green.
3. **Port** tab: choose the folder with the game's NSP, XCI, NSZ or XCZ files (base game, update, DLC) and click **Port**.
4. Tick **Install on the device when done** with the device plugged in and USB debugging on, or copy the APK and the data folder yourself.

A game played by touch: tick **Touch game** on the Port tab (`--touch`). The on-screen gamepad then starts hidden and every touch but one on the menu button goes to the game; the pause menu shows the gamepad again.

A game that crashes with "AllocateAddressRegion … refused (0x1803)" in its log needs more address space: port it again with a larger **Extra address space** on the Port tab (`--alias-headroom` on the command line). [packaging/INSTALL.md](packaging/INSTALL.md#when-something-goes-wrong) says more.

The step-by-step guide is [packaging/INSTALL.md](packaging/INSTALL.md). You need your own game files and `prod.keys`, dumped from your own Switch; the tool never ships or downloads either. NSZ and XCZ files are decompressed during the port (into the build folder's `tmp/`, deleted once extracted).

## Saves

The game's pause menu (Game tab) has **Export save** and **Import save**: an export is a folder in `Android/data/<package>/files/saves/` on the device, the save's files as a console's save tools copy them, so a save from a console imports the same way. The app's **Saves** tab moves them between the device and the PC.

An import replaces the save the game has; the app exports that one first on the device (a folder ending `-before-import`). Uninstalling the app deletes its saves and its exports: copy them to the PC first.

## For developers

Running from the repository, the command line, the repository layout, building the bundle, tests and commit messages: [docs/DEVELOPMENT_GUIDE.md](docs/DEVELOPMENT_GUIDE.md). How it is put together: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Keys and game files

nx2android decrypts your game files with your `prod.keys` while it ports them, as Switch emulators do. This repository and its releases contain no keys, no game files and nothing extracted from them, and the tool never downloads either. Dump both from your own Switch, and don't share them or the APKs and data packs built from them.

Nintendo's takedown notices so far have gone to whoever hosts a project (13 Switch emulator repositories on GitHub in February 2026), not to players. Whether decrypting your own games for your own use is allowed depends on where you live.

## License

MIT, as nxnative (see `nxnative/LICENSE`). Third-party code keeps its own license.
