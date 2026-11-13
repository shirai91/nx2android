"""Build the real Vulkan sampler-limit probe; never contacts a device."""
from pathlib import Path
import argparse
import platform
import subprocess
from test_vk_sampler_limits import production_header, ROOT

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ndk', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
host = {'Darwin': 'darwin-x86_64', 'Linux': 'linux-x86_64'}[platform.system()]
cc = args.ndk / 'toolchains/llvm/prebuilt' / host / 'bin/clang'
args.out.mkdir(parents=True, exist_ok=True)
(args.out / 'sampler_production.h').write_text(production_header())
subprocess.run([str(cc), '--target=aarch64-linux-android31', '-std=gnu11', '-O2', '-g',
                '-DREAL_VULKAN', '-I' + str(args.out), str(ROOT / 'tests/vk_sampler_limits.c'),
                '-lvulkan', '-lm', '-o', str(args.out / 'sampler-limits')], check=True)
