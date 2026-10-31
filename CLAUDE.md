# Rules for Claude in this repository

The skills in `.claude/skills/` (nx2android-dev, switch-android-port) hold the full guide. These rules hold in every session, with or without a skill.

## Game files go only in `.gamedump/`

- Every port, test, check or bundle trial uses `.gamedump/<App>/` in the main checkout (git-ignored; `<App>` is the APK's name, e.g. `HadesII`), also from a worktree. Never make a game, bundle or player folder elsewhere on the PC, and never port into a session's scratchpad (`%TEMP%\claude\...`): `C:\nx2a-test` once kept 263 GB, and old scratchpads 66 GB of ports and pass dumps, that nobody deleted.
- Debug output too big for a few files (pass dumps, frame shots, screen recordings, a port's output) goes in `.gamedump/<App>/` beside the game, and is deleted with it or when the debugging ends.
- Fill it with hard links to the downloaded NSP / NSZ / XCI files (`ln`, same drive, no copy). Never write into the download folders; porter decompresses NSZ and XCZ itself.
- Delete `.gamedump/<App>/` when the work is done (port committed, test app uninstalled). First check that each dump file has 2 or more links (`fsutil hardlink list <file>`); a file with one link is the only copy, so ask first. The PC has little disk space.
- Before ending a session that made a `.gamedump` folder or wrote more than 1 GB anywhere, say which folders are left and why.

## Worktrees

- Always clean up a worktree after its branch is merged into main: remove the worktree and delete the merged branch (`git branch -d`). A session's own worktree goes when the session is archived; say so before ending the session.
- `tools/` (the NDK, the .NET SDK, platform-tools and `prod.keys`) exists only in the main checkout. It is git-ignored, and porter's config points to it by absolute path, so a worktree needs no copy of it. Never make a junction or link to it inside a worktree.
- A link to `tools/` inside a worktree is the main checkout's real folder: a recursive delete of the worktree goes through it and deletes the real files. On 2026-10-31, removing a worktree in Fork deleted the NDK, the .NET SDK and `prod.keys` through a `tools` junction.
- Before you remove a worktree (with git, Fork or any other tool), list its links (`Get-ChildItem -Force <worktree> -Recurse -Attributes ReparsePoint`) and remove each link alone with `cmd /c rmdir <link>` (no `/s`: that removes only the link). Then remove the worktree.

## Releases

- Version `0.MINOR.PATCH` in `porter/__init__.py`: a release with only new games and bug fixes (a core `fix(...)` included) raises the patch; any other core change (a feature or a change in behaviour) raises the minor; the major stays 0 (docs/DEVELOPMENT_GUIDE.md, Releases and version numbers).
- A release checks only the games it adds or fixes. Never port or test the older games again for a release, even after a core change; a break found later is fixed in a later patch release. README.md's Games table: each game's `<sub>` line starts with one tag, the highest version it was tested on.
- Steps: commit `chore(release): version <x.y.z>`, then an annotated tag `v<x.y.z>` on it, push both, then `gh release create v<x.y.z> --verify-tag --notes-file <notes> --repo shirai91/nx2android`, with no zip.
- Never attach or upload the bundle zips by hand. Publishing a release runs `.github/workflows/release-windows.yml` and `release-macos.yml`, which build the zips on GitHub's runners and attach them (`--clobber` replaces a zip of the same name). After publishing, watch both runs and check that both zips are on the release. For a test build of a branch: `gh workflow run release-windows.yml --ref <branch> --repo shirai91/nx2android` (artifact `nx2android-windows`).
- Every `gh` command takes `--repo shirai91/nx2android`: `gh` in this checkout defaults to seedds/nxnative, which is not this project.
- The hook `.claude/hooks/release_guard.py` (`.claude/settings.json`) refuses `gh release create|upload|edit` without that `--repo` or with a zip, and a release tag that is not `v` + the version in `porter/__init__.py`.
