#!/usr/bin/env bash
# gen.sh makes the generated sources it reads (tools/structgen/gen.sh, "EVERY
# SOURCE A RUN READS, MADE FIRST").
#
# A link's sources are not all committed: c/Makefile writes some of them
# (build/gen/cue_takes.c, soundgen's take table, linked into web.wasm and the
# iOS kernel). gen.sh runs before any build, so in a fresh checkout they are
# not there, and structgen stopped on "cannot read --source
# build/gen/cue_takes.c" in every lane that runs gen.sh first.
#
# The check: with every generated source the exports runs list taken away,
# gen.sh --check still succeeds, and each one is there again afterwards. The
# files are moved aside, not deleted, and put back on any exit, so a build in
# this tree is left as it was.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
gen="${GEN_SH:-$here/../gen.sh}"   # GEN_SH: a copy with the promise broken, to test this test
# The generated sources: what c/Makefile writes under build/ for a source list.
gen_srcs=(build/gen/cue_takes.c)
c="$root/c"
t="$(mktemp -d)"
restore() {
  local f
  for f in "${gen_srcs[@]}"; do
    if [ -e "$t/$(basename "$f")" ] && [ ! -e "$c/$f" ]; then mkdir -p "$(dirname "$c/$f")"; mv "$t/$(basename "$f")" "$c/$f"; fi
  done
  rm -rf "$t"
}
trap restore EXIT
for f in "${gen_srcs[@]}"; do
  if [ -e "$c/$f" ]; then mv "$c/$f" "$t/"; fi
  [ ! -e "$c/$f" ] || { echo "not ok - $f could not be taken away"; exit 1; }
done

cd "$root"
if ! bash "$gen" --check > "$t/log" 2>&1; then
  echo "not ok - gen.sh --check failed with its generated sources missing:"
  tail -5 "$t/log" | sed 's/^/      /'
  exit 1
fi
for f in "${gen_srcs[@]}"; do
  [ -s "$c/$f" ] || { echo "not ok - gen.sh --check left c/$f missing"; exit 1; }
done
echo "ok - gen.sh --check makes its generated sources (${gen_srcs[*]})"
