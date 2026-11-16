#!/bin/bash
# Stage the Linux dmhost as a nix store path and copy it to the Nova, then point the launcher at it.
# usage: devtools/linux_deploy_nova.sh [titleid]   (default 0100DBF01000A000, Burnout Paradise Remastered)
# Needs: the build done (nxnative/dm/linux/build.py ... --rpath ...), nix with flakes, ssh heath@nova.lan, a launcher at ~/Games/burnout-linux/burnout on the device.
set -e
export NIX_CONFIG="experimental-features = nix-command flakes"
ID=${1:-0100DBF01000A000}
HERE=$(cd "$(dirname "$0")/.." && pwd)
STAGE=$(mktemp -d)   # a path without spaces: nix path literals cannot take them
cp "$HERE/nxnative/dm/linux/out/$ID/dmhost" "$STAGE/dmhost"
# the pinned nixpkgs of nova-nix, so the glibc matches the device (check: nova-nix flake.lock nixpkgs rev)
OUT=$(nix build --impure --no-link --print-out-paths --expr "let pkgs = (builtins.getFlake \"nixpkgs\").legacyPackages.aarch64-linux; in pkgs.runCommand \"dmhost-$ID\" {} ''install -Dm755 \${builtins.path { path = $STAGE/dmhost; name = \"dmhost\"; }} \$out/bin/dmhost''" | tail -1)
nix copy --to ssh://heath@nova.lan "$OUT"
ssh heath@nova.lan "sed -i 's|^exec .*dmhost mods|exec $OUT/bin/dmhost mods|' ~/Games/burnout-linux/burnout; grep ^exec ~/Games/burnout-linux/burnout"
rm -rf "$STAGE"
