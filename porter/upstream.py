# nxnative's own tools, used as libraries: the NSP script's steps (scripts/build_ipa_from_nsp.py: NSP scan, extraction, rewrite, shader split, stage reuse, progress), the game profiles
# (dm/games/game.py), the guesses from game files (dm/games/profile_guess.py), the control data reader (dm/tools/nacp.py), the Android profile (dm/android/tools/android_profile.py) and
# Game Freak's Trinity file system (dm/gfx/tools/trinity.py) and Unreal Engine's paks and IoStore containers (dm/gfx/tools/uepak.py).
# Importing them instead of copying keeps one implementation of each step for iOS and Android; what differs on Android (the rewrite target, the Vulkan shaders, the host, the APK)
# is in porter.pipeline.
import importlib.util, sys

from porter.paths import DM, NXNATIVE

for d in (DM / 'games', DM / 'tools', DM / 'android/tools', DM / 'gfx/tools'):
    if str(d) not in sys.path: sys.path.insert(0, str(d))

import android_profile   # noqa: E402
import game              # noqa: E402
import nacp              # noqa: E402
import profile_guess     # noqa: E402
import trinity           # noqa: E402
import uepak             # noqa: E402


def _load_nsp():
    spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', NXNATIVE / 'scripts/build_ipa_from_nsp.py')
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    m.nacp, m.pg = nacp, profile_guess   # module globals its main() sets
    if sys.platform != 'darwin': m.can_clone = lambda d: False   # APFS clones (clonefile) exist only on a Mac
    files = m.profile_files   # the shader stages' reuse keys: android.tsv changes no shader (an edit to it re-split and re-translated Viractal's 25,464 shaders)
    m.profile_files = lambda g: [p for p in files(g) if p.name != 'android.tsv']
    return m


nsp = _load_nsp()
BuildError, PROG, say = nsp.BuildError, nsp.PROG, nsp.say
