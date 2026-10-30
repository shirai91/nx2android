# nx2android

Port your own Nintendo Switch games to Android, natively: the game's own arm64 code runs on the device, with the Switch's system services stood in for by a host and NVN graphics replayed on Vulkan. Pick the game's folder, click **Port**, and get an APK plus the game's data, installed on your device if you like. Bring your own game files and keys; nothing from any game is in this repository.

This is the Android port of [nxnative](https://github.com/seedds/nxnative), which does the same on iPhone and iPad. Supported games: [Games](#games).

## Games

These games play on Android. Tested on a Lenovo Legion Y700 (Snapdragon 8 Elite, Adreno 830). The device's GPU must read BC-compressed textures (Adreno, Xclipse; not Mali or PowerVR).

| | |
|:---:|:---:|
| ![13 Sentinels: Aegis Rim](docs/images/13-sentinels.jpg)<br>13 Sentinels: Aegis Rim<br><sub>update 1.10 · 60 fps</sub> | ![Advance Wars 1+2: Re-Boot Camp](docs/images/advance-wars-1-2-re-boot-camp.jpg)<br>Advance Wars 1+2: Re-Boot Camp<br><sub>base game 1.0.0 · 60 fps (30 in battle on Switch)</sub> |
| ![Blasphemous 2](docs/images/blasphemous-2.jpg)<br>Blasphemous 2<br><sub>update 1.0.8 · 60 fps</sub> | ![Captain Toad: Treasure Tracker](docs/images/captain-toad-treasure-tracker.jpg)<br>Captain Toad: Treasure Tracker<br><sub>update 1.4.2 · 60 fps</sub> |
| ![Castlevania: Belmont's Curse](docs/images/castlevania-belmonts-curse.jpg)<br>Castlevania: Belmont's Curse<br><sub>update 1.1.0 · 60 fps</sub> | ![Celeste](docs/images/celeste.jpg)<br>Celeste<br><sub>update 1.4.1.1 · 60 fps</sub> |
| ![Donkey Kong Country Returns HD](docs/images/donkey-kong-country-returns-hd.jpg)<br>Donkey Kong Country Returns HD<br><sub>update 1.1.0 · 60 fps</sub> | ![Graveyard Keeper 2](docs/images/graveyard-keeper-2.jpg)<br>Graveyard Keeper 2<br><sub>update 1.0.4 · 60 fps (30 on Switch)</sub> |
| ![Hades II](docs/images/hades-ii.jpg)<br>Hades II<br><sub>update 1.0 · 60 fps</sub> | ![Hollow Knight: Silksong](docs/images/hollow-knight-silksong.jpg)<br>Hollow Knight: Silksong<br><sub>update 1.0.30000 · 60 fps</sub> |
| ![Mario vs. Donkey Kong](docs/images/mario-vs-donkey-kong.jpg)<br>Mario vs. Donkey Kong<br><sub>update 1.0.1 · 60 fps</sub> | ![Metroid Dread](docs/images/metroid-dread.jpg)<br>Metroid Dread<br><sub>update 2.1.0 · 60 fps</sub> |
| ![NEO: The World Ends with You](docs/images/neo-the-world-ends-with-you.jpg)<br>NEO: The World Ends with You<br><sub>update 1.03</sub> | ![Paper Mario: The Origami King](docs/images/paper-mario-the-origami-king.jpg)<br>Paper Mario: The Origami King<br><sub>update 1.0.1 · 60 fps (30 on Switch)</sub> |
| ![Pokémon Legends: Arceus](docs/images/pokemon-legends-arceus.jpg)<br>Pokémon Legends: Arceus<br><sub>update 1.1.1 · 60 fps (30 on Switch)</sub> | ![Super Mario Bros. Wonder](docs/images/super-mario-bros-wonder.jpg)<br>Super Mario Bros. Wonder<br><sub>update 1.2.1 · 60 fps, dips to 40</sub> |
| ![Super Mario Odyssey](docs/images/super-mario-odyssey.jpg)<br>Super Mario Odyssey<br><sub>update 1.4.1 · 60 fps · shake the device to throw Cappy</sub> | ![Tales of Eternia Remastered](docs/images/tales-of-eternia-remastered.jpg)<br>Tales of Eternia Remastered<br><sub>update 2.0.0 · 60 fps</sub> |
| ![The Legend of Zelda: Link's Awakening](docs/images/the-legend-of-zelda-links-awakening.jpg)<br>The Legend of Zelda: Link's Awakening<br><sub>update 1.1.0 · 60 fps · **work in progress**: played in the Android Emulator only, not on a phone or tablet yet</sub> | ![Unicorn Overlord](docs/images/unicorn-overlord.jpg)<br>Unicorn Overlord<br><sub>update 1.05 · 60 fps · **boots**: saving and a full battle not tested yet</sub> |
| ![Unpacking](docs/images/unpacking.jpg)<br>Unpacking<br><sub>update 2.01 · 60 fps · played by touch</sub> | ![Viractal: Will You Trust Your Party?](docs/images/viractal.jpg)<br>Viractal: Will You Trust Your Party?<br><sub>update 1.4.5</sub> |
| ![Xenoblade Chronicles 3](docs/images/xenoblade-chronicles-3.jpg)<br>Xenoblade Chronicles 3<br><sub>update 2.2.0 · 30 fps · **work in progress**: some effects and models don't draw yet</sub> | |

A game that is not in this table is not supported yet. Which games each release has been checked to port: [docs/RELEASE_CHECK.md](docs/RELEASE_CHECK.md). The games we look at next, and the ones done: [docs/PORT_CHECKLIST.md](docs/PORT_CHECKLIST.md).

## For players: port a game without programming

1. Download `nx2android-<version>-windows-x64.zip` from [Releases](https://github.com/shirai91/nx2android/releases/latest), unzip it and start **nx2android.exe**.
2. **Setup** tab: click **Download missing tools** (the Android NDK, the .NET SDK and platform-tools, about 1 GB, from Google and Microsoft), then set your **prod.keys**. Every line turns green.
3. **Port** tab: choose the folder with the game's NSP, XCI, NSZ or XCZ files (base game, update, DLC) and click **Port**.
4. Tick **Install on the device when done** with the device plugged in and USB debugging on, or copy the APK and the data folder yourself.

The step-by-step guide is [packaging/INSTALL.md](packaging/INSTALL.md). You need your own game files and `prod.keys`, dumped from your own Switch; the tool never ships or downloads either. NSZ and XCZ files are decompressed during the port (into the build folder's `tmp/`, deleted once extracted).

## For developers

Running from the repository, the command line, the repository layout, building the bundle, tests and commit messages: [docs/DEVELOPMENT_GUIDE.md](docs/DEVELOPMENT_GUIDE.md). How it is put together: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Keys and game files

nx2android decrypts your game files with your `prod.keys` while it ports them, as Switch emulators do. This repository and its releases contain no keys, no game files and nothing extracted from them, and the tool never downloads either. Dump both from your own Switch, and don't share them or the APKs and data packs built from them.

Nintendo's takedown notices so far have gone to whoever hosts a project (13 Switch emulator repositories on GitHub in February 2026), not to players. Whether decrypting your own games for your own use is allowed depends on where you live.

## License

MIT, as nxnative (see `nxnative/LICENSE`). Third-party code keeps its own license.
