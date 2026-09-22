#!/bin/sh
# The properties the win-probability wire rests on, checked against a real game.
#
#   make -C c winprob && c/tools/winprob/checks.sh <replay code or link>
#
# The bytes are compared through the --tsv dump, which is read back out of them
# by the reader, with elapsed_ms filtered: wall time is the one header field
# that is not a function of the inputs.
#
# Every check here was mutation-checked - the thing it guards was broken on
# purpose, the check was seen to fail, and the break was reverted:
#   the strategy RNG seeded per worker thread   -> one thread != eight
#   the reader's length check removed           -> a cut file read as whole
#   the step stride moved by one byte           -> the run refuses to write
set -e
cd "$(dirname "$0")/../../.."
CODE=${1:?usage: checks.sh <replay code or foolish.cards link>}
BIN=./c/build/cnitro_winprob
COMMON="--engine=handwritten --worlds=40 --belief-worlds=8 --quiet"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

run() {
  $BIN --code="$CODE" $COMMON "$2" --tsv="$1" >/dev/null
  grep -v '^elapsed_ms' "$1" > "$1.cmp"
}

run "$OUT/t1.tsv"  --threads=1
run "$OUT/t8.tsv"  --threads=8
run "$OUT/t8b.tsv" --threads=8
run "$OUT/s2.tsv"  --seed=2

if ! diff -q "$OUT/t1.tsv.cmp" "$OUT/t8.tsv.cmp"; then
  echo "FAIL one thread != eight threads"; exit 1
fi
echo "PASS one thread == eight threads"

if ! diff -q "$OUT/t8.tsv.cmp" "$OUT/t8b.tsv.cmp"; then
  echo "FAIL the same run twice gave different bytes"; exit 1
fi
echo "PASS same run twice == same bytes"

if diff -q "$OUT/t8.tsv.cmp" "$OUT/s2.tsv.cmp" >/dev/null 2>&1; then
  echo "FAIL a different seed gave identical bytes"; exit 1
fi
echo "PASS a different seed moves the bytes"

# The reader refuses a truncated file rather than reading a shorter strip.
$BIN --code="$CODE" $COMMON --threads=8 --bin="$OUT/full.bin" >/dev/null
SIZE=$(wc -c < "$OUT/full.bin" | tr -d ' ')
head -c $((SIZE - 1)) "$OUT/full.bin" > "$OUT/cut.bin"
if ! python3 c/tools/winprob/winprob_bin.py "$OUT/full.bin" >/dev/null; then
  echo "FAIL the whole file did not read"; exit 1
fi
echo "PASS the whole file reads"
if python3 c/tools/winprob/winprob_bin.py "$OUT/cut.bin" >/dev/null 2>&1; then
  echo "FAIL a truncated file read as a whole one"; exit 1
fi
echo "PASS a truncated file is refused"
