#!/usr/bin/env bash
# gen.sh writes a module only when its content changed (tools/structgen/gen.sh,
# "EVERYTHING IS GENERATED INTO A STAGING TREE").
#
# That is the property the iOS build leans on: it runs gen.sh first on every
# build, and Xcode recompiles whatever imports a Swift file whose modification
# time moved. A gen.sh that rewrote its outputs in place would turn every build
# into a rebuild of the kit, which is why the build did not call it before.
#
# Three checks, over the real outputs of the real script:
#   1. a run over an unchanged tree touches no file;
#   2. a module that is MISSING is written back;
#   3. a module whose bytes are WRONG is put right - and in both of those
#      runs, nothing else is touched.
#
# Files are compared to a marker with `find -newer`, which is the same on a Mac
# and on Linux; the one-second sleeps are what make "newer" mean something on a
# filesystem that keeps whole seconds.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
gen="${GEN_SH:-$here/../gen.sh}"   # GEN_SH: a copy with one promise broken, to test this test
cd "$root"

dirs=$(bash "$gen" --print-dirs)
t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
fails=0
# What a run of gen.sh wrote: the files newer than the marker, sorted.
touched() {  # shellcheck disable=SC2086
  find $dirs -type f -newer "$t/marker" | sort | tr '\n' ' ' | sed 's/ $//'
}
expect() {  # <what is promised> <expected files, sorted>
  local got; got=$(touched)
  if [ "$got" = "$2" ]; then echo "ok - $1"
  else fails=$((fails+1)); echo "not ok - $1"; echo "      touched:  ${got:-nothing}"; echo "      expected: ${2:-nothing}"; fi
}
mark() { touch "$t/marker"; sleep 1; }

bash "$gen"                       # whatever state the tree was in, it is current now
mark; bash "$gen"
expect "a run over an unchanged tree touches no file" ""

gone="sdk/swift/gen/i18n/FoolishStringKeys.swift"
[ -f "$gone" ] || { echo "publish.sh: $gone is not a generated file any more - pick another"; exit 2; }
rm "$gone"
mark; bash "$gen"
expect "a missing module is written back, and nothing else is touched" "$gone"

wrong="sdk/ts/gen/i18n/keys.ts"
[ -f "$wrong" ] || { echo "publish.sh: $wrong is not a generated file any more - pick another"; exit 2; }
cp "$wrong" "$t/keys.ts"
echo "// not what the generator writes" >> "$wrong"
mark; bash "$gen"
expect "a module with the wrong bytes is put right, and nothing else is touched" "$wrong"
cmp -s "$wrong" "$t/keys.ts" && echo "ok - ...and it is the generator's bytes again" \
  || { fails=$((fails+1)); echo "not ok - $wrong was touched but is still not the generator's bytes"; }

if [ "$fails" = 0 ]; then echo "publish.sh: all checks passed"; else echo "publish.sh: $fails FAILED"; exit 1; fi
