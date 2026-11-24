<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
<!-- end of the game facts -->

<!--
The page of one ported game: nxnative/dm/games/<title ID>/README.md. It is for players first: what works, on what, and how to port it well.
docs/gen_tables.py makes it from this template when the profile has readme_image and readme_tag rows and no README.md yet, and writes the block
above (title, picture, status, tested version, what the profile sets) from android.tsv on every run. Write the sections below by hand; keep their
headings (the check fails without them, or with a TODO: line left). Add sections of your own under them if the game needs more
(a route to test, how to play by touch, porting notes for developers). The long porting log stays in android.tsv's comment header.
Delete this comment.
-->

## Game version

- TODO: the game update the port was tested with (`update 1.0.2`, or `base game 1.0.0`), and other updates known to work or to fail.

## Frame rate

- TODO: what the game holds on the tested device (`60 fps`, `30 fps (as on Switch)`, `60 fps (30 on Switch)`), and where it dips.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| TODO: Lenovo Legion Y700 (TB322FC) | Adreno 830 | system | 1.0.2 | `v0.5.0` | plays: the title, a new save, the first area |

## Options

- TODO: port options to set (`--vulkan-driver`, `--firmware`, `--language`, the GUI's fields) and the in-game or pause menu settings that play best. Write "None: the default options work." when there are none.

## Quirks and known issues

- TODO: what does not work or differs from the Switch, and how to get past it. Write "None known." when there are none.
