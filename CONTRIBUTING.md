# Contributing to nx2android

Thanks for helping. Read [docs/DEVELOPMENT_GUIDE.md](docs/DEVELOPMENT_GUIDE.md) for the setup, the tests and the commit message rules.

## Test on a device before you open a pull request

Every pull request needs at least one run on a real Android device, done by its author, before it is opened. A pull request without one is closed.

The pull request text names:

- the device and its GPU (for example Legion Y700, Adreno 830);
- the Vulkan driver (the Qualcomm system driver or Turnip, with its version);
- the game, its title ID and its version (for example 1.1.0, `v131072`);
- the nx2android commit you ported with;
- what you played, or the scene that the change fixes, on main and with the change;
- the proof: the port log or app log lines, and frame shots or a short recording for a picture change.

Community testers confirm and widen a tested result: more devices, more drivers, more games. They do not do the first test for the author. Asking for testers in place of your own run is not enough.

The device run is in addition to the tests in `tests/`, which every pull request must also pass.

### Game profiles

An `android.tsv` means that the game was brought up on an Android device. A new profile needs your own device run of the build it registers, a `tested` row, the game's page `nxnative/dm/games/<title ID>/README.md` and a gameplay frame captured on the device ([DEVELOPMENT_GUIDE.md, Adding a game](docs/DEVELOPMENT_GUIDE.md#adding-a-game)). To try a game that nobody has run yet, use `porter port --experimental`; do not open a profile pull request for it.

### Core changes

A core change (runtime, Vulkan backend, shader translator, rewriter, porter) names a game, its version and a scene that is wrong on Android on main, and shows that the change fixes it on your device. A change that only matches another project's behaviour, with no game that fails on Android, is not taken.

If the change alters shader output or rewrites modules for games that are already shipped, say which shipped game you checked and how many shaders or words changed for it.

### Code from nxnative

nx2android is an independent fork. Code adapted from [seedds/nxnative](https://github.com/seedds/nxnative) keeps its credit: add `Adapted from seedds/nxnative <commit> under the MIT licence` to the commit body and the pull request text. The device-test rule applies to it in the same way.

## Size and scope

- One change for each pull request. Split unrelated fixes, even when they come from the same source.
- Keep the pull request's text to what you did and what you saw on the device. Do not claim results that you did not measure.
