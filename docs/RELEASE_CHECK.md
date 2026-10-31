# Release check

Which games the player's bundle has been checked to port, from the game files to an APK and a data pack, on a Windows PC with nothing from the developer's machine: the bundle unzipped into an empty folder, its own player folder, the tools from its **Download missing tools**, and a game folder holding only the game's NSP files. Which games play is in the README's [Games](../README.md#games).

A release ships when the games it adds, and the games it fixes, have been ported by the bundle it ships.

## Older games are not checked again

Since 2026-10-31 a release does not port the older games again, even after a core change: there are too many games to test each one for each release. Their rows below stay from the bundle that last ported them. A break that a player or a later port finds in an older game is fixed in a later patch release, which then checks that game again.

Which nx2android version each game was last tested on is the tag in the README's [Games](../README.md#games) table: one tag, the highest version. A release sets the tag of each game it checks to its own version.

## When to run the check

| What changed | Run the check for |
|---|---|
| A new game profile (`nxnative/dm/games/<title ID>/` only) | that game: add its row |
| A game's profile (its `android.tsv`, `game.tsv`, `patches.py`) | that game |
| A fix for a game that broke | that game |
| The core (`porter/`, `packaging/`, `nxnative/dm/{host,gfx,tools,android,third_party}`, `nxnative/scripts/`), or the NDK or .NET version | only the games above; the Results paragraph names the core commits the older rows have not seen |
| `gui/` or docs only | nothing (start the app once) |

## Version number

The version is `0.MINOR.PATCH` (`porter/__init__.py`, commit `chore(release): version <x.y.z>`). The major version stays 0 while the project is early.

- **Patch** (0.1.3 → 0.1.4): the release only adds games and fixes bugs, in profiles or in the core.
- **Minor** (0.1.3 → 0.2.0): the release changes the core (the paths in the table above) other than to fix a bug: a new feature or a change in behaviour, such as the save manager or the device Oodle decoder.

## How

1. Build the bundle (`python packaging/build_bundle.py`) and unzip it into an empty folder, say `C:\nx2a-test\nx2android`.
2. Once per test folder, with a player folder of its own: `set NX2A_HOME=C:\nx2a-test\home`, then `nx2android.exe -m porter tools --download` and `nx2android.exe -m porter config keys <prod.keys>`.
3. Give each game a folder of hard links to its NSP files (`mklink /H <new> <nsp>`, or `ln` in Git Bash): a port from scratch without copying the game.
4. `python devtools/port_check.py C:\nx2a-test\nx2android <game folder> … --fresh --write` ports each one and puts its row below, replacing that game's earlier row.

## Results

The v0.1.0 check (2026-10-27): bundle `7d2b374` ported four games and stopped on NEO, whose committed shader tables lacked the 20 Unity compute shaders the split reads since `386efda`. Bundle `e89aecb` differs from it only in NEO's profile tables and ported NEO. v0.1.0 differs from `e89aecb` only in documents. The v0.1.0 NEO port, installed beside the player's own as `org.nx2a.neo.test`, then played on the Legion Y700: through the DLC gifts and the opening, into free roam at Scramble Crossing with its crowd, at 40-60 fps, with no new `[RED]` lines.

The v0.1.1 check (2026-10-27): bundle `bd68d8b` (v0.1.0-5-gbd68d8b) ported Hades II, the game it adds, from scratch. By the maintainer's choice only the new game was checked for this release: the Vulkan backend changes it brings (`c9c9e38`) were not run through the check for the other five games, whose rows are still from v0.1.0's bundles. v0.1.1 differs from `bd68d8b` only in documents. The release build installed on the Legion Y700 (`org.nx2a.hades2`) was ported from the development checkout at `75a1a18`, the same sources but for the version number, and starts at the title at 60 fps.

The Tales of Eternia Remastered check (2026-10-27): bundle `8372959` (v0.1.1-1-g8372959) ported it, the game it adds, from scratch. The same commit makes AAudio call back every 5 ms for every game (`audiotoolbox_aaudio.c`); the other games' rows are still from earlier bundles, and only Tales of Eternia was played with that change.

The v0.1.2 release (2026-10-28): by the maintainer's choice no bundle was run through the check. The maintainer instead rebuilt every game from the development checkout while testing on 2026-10-28, and these were played on the Legion Y700 that day: NEO, Hades II, Unpacking and Tales of Eternia from a fresh build folder at `21a0d68` (60 fps), Graveyard Keeper 2 at 60 fps (`21a0d68`), Mario vs. Donkey Kong (`d84dca1`). The core changes since v0.1.1 that touch every game were therefore not run through the check: the display-timed presents (`4802f3c`), the frame rate governor (`dba318d`), the node area cut on the sdk build Graveyard Keeper 2, Tales of Eternia and Viractal share (`8056043`), AAudio's 5 ms callbacks (`8372959`), the FPS counter (`27b96fc`), and the porter's Windows fixes (`e490097`, `2ee2723`). Mario vs. Donkey Kong has no row below: no bundle has ported it yet.

The v0.1.3 check (2026-10-29): bundle `27ea47f` (v0.1.2-71-g27ea47f) ported from scratch the nine games v0.1.3 adds (13 Sentinels, Advance Wars 1+2, Blasphemous 2, Captain Toad, Celeste, Donkey Kong Country Returns HD, Hollow Knight: Silksong, Super Mario Bros. Wonder, Unicorn Overlord) and Mario vs. Donkey Kong, which had no row; Blasphemous 2, Captain Toad, Celeste and Silksong from NSZ dumps. By the maintainer's choice the seven older games were not ported again: they were played during development, and their rows are still from earlier bundles. So the core changes since v0.1.2 that touch every game have been through the check only with the ten games above: the present flip for a lower-left origin (`46bc97b`), the sRGB swapchain (`e27a2d4`), viewport swizzles (`b856346`), the depth-only LRZ read (`79e7074`), zero-copy pool chunks over 256 MB (`56ff85b`) and the pool search (`9ad3165`), Adreno framebuffer compression by default (`a1b4f54`), CPU-written linear render targets (`af9d21b`), the rewriter's tail call (`bdfaa02`), the SDK 15.3.2 sdk cuts (`3200036`), Vulkan 1.1 devices (`2bab15b`), the kept AAudio stream (`debdb60`, `da3f88b`), the focus after a trip home (`ce52af2`), the RomFS name lookup (`a622ac4`), NSZ / XCZ dumps (`075f905`) and the themed gamepad (`92c88b9`). The experimental Unicorn Overlord (Asia) profile (`010054B01AD92000`) was not checked: there is no dump of it on the check PC.

<!-- port_check rows -->
| Game | Title ID | Version | Result | Output | Time | Bundle | Date |
|---|---|---|---|---|---|---|---|
| 13 Sentinels: Aegis Rim | `01003FC01670C000` | 1.10 | ported | APK 2 MB, data 4.4 GB | 1 min (from scratch) | v0.1.2-71-g27ea47f | 2026-10-29 |
| Advance Wars 1+2: Re-Boot Camp | `0100300012F2A000` | 1.0.0 | ported | APK 2 MB, data 7.2 GB | 4 min (from scratch) | v0.1.2-71-g27ea47f | 2026-10-29 |
| Blasphemous 2 | `010040801A4C8000` | 1.0.8 | ported | APK 2 MB, data 1.9 GB, 1 DLC | 3 min (from scratch) | v0.1.2-71-g27ea47f | 2026-10-29 |
| Captain Toad: Treasure Tracker | `01009BF0072D4000` | 1.4.2 | ported | APK 2 MB, data 1.9 GB, 2 DLC | 2 min (from scratch) | v0.1.2-71-g27ea47f | 2026-10-29 |
| Celeste | `01002B30028F6000` | 1.4.1 | ported | APK 2 MB, data 1.2 GB | 2 min (from scratch) | v0.1.2-71-g27ea47f | 2026-10-29 |
| Donkey Kong Country Returns HD | `01009D901BC56000` | 1.1.0 | ported | APK 2 MB, data 13.1 GB | 6 min (from scratch) | v0.1.2-71-g27ea47f | 2026-10-29 |
| Graveyard Keeper 2 | `0100005027A18000` | 1.0.4 | ported | APK 1 MB, data 1.4 GB, 1 DLC | 4 min (from scratch) | 7d2b374 | 2026-10-27 |
| Hades II | `0100A00019DE0000` | 1.0.141667 | ported | APK 2 MB, data 4.7 GB | 1 min (from scratch) | v0.1.0-5-gbd68d8b | 2026-10-27 |
| Hollow Knight: Silksong | `010013C00E930000` | 1.0.30000 | ported | APK 2 MB, data 6.5 GB | 5 min (from scratch) | v0.1.2-71-g27ea47f | 2026-10-29 |
| Mario vs. Donkey Kong | `0100B99019412000` | 1.0.1 | ported | APK 2 MB, data 1.6 GB | 4 min (from scratch) | v0.1.2-71-g27ea47f | 2026-10-29 |
| NEO: The World Ends with You | `010043B013C5C000` | 1.03 | ported | APK 2 MB, data 6.7 GB, 2 DLC | 3 min (from scratch) | e89aecb | 2026-10-27 |
| Super Mario Bros. Wonder | `010015100B514000` | 1.2.1 | ported | APK 2 MB, data 4.0 GB | 4 min (from scratch) | v0.1.2-71-g27ea47f | 2026-10-29 |
| Tales of Eternia Remastered | `010006E0230AC000` | 2.0.0 | ported | APK 2 MB, data 5.6 GB | 4 min (from scratch) | v0.1.1-1-g8372959 | 2026-10-27 |
| Unicorn Overlord | `010069401ADB8000` | 1.05 | ported | APK 2 MB, data 3.5 GB, 2 DLC | 1 min (from scratch) | v0.1.2-71-g27ea47f | 2026-10-29 |
| Unpacking | `0100701015846000` | 2.01 | ported | APK 2 MB, data 1.0 GB | 2 min (from scratch) | 7d2b374 | 2026-10-27 |
| Viractal | `0100FD501F95A000` | 1.4.5 | ported | APK 2 MB, data 2.3 GB | 5 min (from scratch) | 7d2b374 | 2026-10-27 |
| Xenoblade Chronicles 3 | `010074F013262000` | 2.2.0 | ported | APK 2 MB, data 19.2 GB, 4 DLC | 4 min (from scratch) | 7d2b374 | 2026-10-27 |
<!-- /port_check rows -->
