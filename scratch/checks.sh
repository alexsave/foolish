#!/bin/sh
# The properties the wire rests on, checked against the real game.
#
# The bytes are compared through the --tsv dump, which is read back out of them
# by the reader, with elapsed_ms filtered: wall time is the one header field
# that is not a function of the inputs.
set -e
cd /Users/alex/Dev/foolish/.claude/worktrees/replay-winprob
CODE=$(tr -d '\n' < scratch/code.txt)
BIN=./c/build/cnitro_winprob
COMMON="--engine=handwritten --worlds=40 --belief-worlds=8 --quiet"

run() { $BIN --code="$CODE" $COMMON "$2" --tsv="$1" >/dev/null; grep -v '^elapsed_ms' "$1" > "$1.cmp"; }

run scratch/t1.tsv --threads=1
run scratch/t8.tsv --threads=8
run scratch/t8b.tsv --threads=8
run scratch/s2.tsv --seed=2

if ! diff -q scratch/t1.tsv.cmp scratch/t8.tsv.cmp; then
  echo "FAIL one thread != eight threads"; exit 1
fi
echo "PASS one thread == eight threads"
if ! diff -q scratch/t8.tsv.cmp scratch/t8b.tsv.cmp; then
  echo "FAIL the same run twice gave different bytes"; exit 1
fi
echo "PASS same run twice == same bytes"
if diff -q scratch/t8.tsv.cmp scratch/s2.tsv.cmp >/dev/null 2>&1; then
  echo "FAIL a different seed gave identical bytes"; exit 1
fi
echo "PASS a different seed moves the bytes"

# The reader refuses a truncated file rather than reading a shorter strip.
$BIN --code="$CODE" $COMMON --threads=8 --bin=scratch/full.bin >/dev/null
SIZE=$(wc -c < scratch/full.bin | tr -d ' ')
head -c $((SIZE - 1)) scratch/full.bin > scratch/cut.bin
if ! python3 c/tools/winprob/winprob_bin.py scratch/full.bin >/dev/null; then
  echo "FAIL the whole file did not read"; exit 1
fi
echo "PASS the whole file reads"
if python3 c/tools/winprob/winprob_bin.py scratch/cut.bin >/dev/null 2>&1; then
  echo "FAIL a truncated file read as a whole one"; exit 1
fi
echo "PASS a truncated file is refused"
