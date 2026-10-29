#!/bin/bash
# retake.sh <commit> [tool and its arguments]: the table as it was at <commit>, to take a README
# number again. The commit is checked out in a clean worktree of this repo (detached, outside this
# checkout), built there with its own build.sh, and its out/ copied to out_<commit>/ here (index.html
# and the wasm: serve foolish/ and open /c/cdom_poc/out_<commit>/index.html, or pass PAGE= to a
# tool that takes it). With a tool (for example tools/timing.mjs 21 1), that commit's own copy of
# the tool is run in the worktree against that build, as the number was first taken. The worktree
# is removed after, whatever happens.
#   tools/retake.sh df7fda82d tools/timing.mjs 21 1
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
repo="$(git -C "$here" rev-parse --show-toplevel)"
sha="$(git -C "$repo" rev-parse --short=9 "$1")"
shift
wt="$(mktemp -d "${TMPDIR:-/tmp}/cdom-retake-XXXXXX")"
rmdir "$wt"
git -C "$repo" worktree add --detach "$wt" "$sha" > /dev/null 2>&1
trap 'git -C "$repo" worktree remove --force "$wt"' EXIT
poc="$wt/foolish/c/cdom_poc"
ln -s "$here/tools/node_modules" "$poc/tools/node_modules"
(cd "$poc" && ./build.sh > "$wt/build.log" 2>&1) || { echo "$sha: the build failed"; tail -20 "$wt/build.log"; exit 1; }
rm -rf "$here/out_$sha"
mkdir -p "$here/out_$sha"
for f in index.html table.wasm table_debug.wasm; do
  if [ -f "$poc/out/$f" ]; then cp "$poc/out/$f" "$here/out_$sha/"; fi
done
echo "$sha: $(tail -1 "$wt/build.log") -> out_$sha/"
if [ $# -gt 0 ]; then
  (cd "$poc" && node "$@")
fi
