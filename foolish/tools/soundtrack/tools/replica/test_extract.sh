#!/bin/bash
# test_extract.sh - end to end: synthetic song -> extract.py per stem -> play.c -> compare.py, plus the exact-flag check.
# What it does: builds a 12 s swung, off-grid song as three stems, extracts samples and events at distance 0.05, writes
#   score.txt and parts.txt, compiles replica/play.c, renders the mix and each part solo, and asserts (see test_extract.py).
# Inputs: none. Optional environment, for mutation checks: EXTRACT (extract.py to test), ASSEMBLE (assemble.py of the pitch fixtures), PLAYC (play.c to compile; its
#   ../engine must sit beside it), TMP (work directory). Outputs: lines with the measured numbers; exit 1 on any failure.
# How to run: bash test_extract.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
EXTRACT=${EXTRACT:-$HERE/extract.py}
PLAYC=${PLAYC:-$HERE/../../replica/play.c}
ENGINE_DIR=$(cd "$(dirname "$PLAYC")/../engine" && pwd)
TMP=${TMP:-$(mktemp -d)}
PY="python3 -I"
fail=0
mkdir -p "$TMP/orig" "$TMP/rep"
echo "work dir $TMP"
$PY "$HERE/test_extract.py" make "$TMP/orig" || exit 2
PARTS=""
for s in drums bass other; do
  S=$(echo $s | tr a-z A-Z)
  $PY "$EXTRACT" --stem "$TMP/orig/$s.wav" --name $S --start 0 --end 12 --bpm 120 --distance 0.05 ${EXTRA:-} \
    --samples-dir "$TMP/rep/samples_$s" --manifest "$TMP/rep/man_$s.txt" --events "$TMP/rep/ev_$s.txt" --table "$TMP/rep/tab_$s.tsv" || exit 2
  PARTS="$PARTS --part $S $TMP/rep/ev_$s.txt $TMP/rep/man_$s.txt $s"
done
$PY "$HERE/assemble.py" --dir "$TMP/rep" --bpm 120 --bars 6 $PARTS || exit 2
clang -O2 -w -o "$TMP/play" "$PLAYC" || exit 2
cd "$TMP/rep" || exit 2
export SAMPLES=/nonexistent ENGINE=/nonexistent
unset SOLO
"$TMP/play" full out 2> render.log || { cat render.log; exit 2; }
grep -E "engine sample rate|master:" render.log
MG=$(sed -n 's/.*master gain \(-\{0,1\}[0-9.]*\) dB.*/\1/p' render.log)
for s in drums bass other; do
  mkdir -p "solo_$s"
  # a solo render is unnormalised; the mix's master gain makes it the same scale as the mix
  ( cd "$TMP/rep" && SOLO=$s MGAIN=$MG "$TMP/play" full "solo_$s" 2> "solo_$s.log" ) || { cat "solo_$s.log"; exit 2; }
done
$PY "$HERE/test_extract.py" check "$TMP" || fail=1
# the lead: isolated bursts with --lead-ms 6 and 12
$PY "$HERE/test_extract.py" leadmake "$TMP/lead" || exit 2
for L in 6 12; do
  $PY "$EXTRACT" --stem "$TMP/lead/bursts.wav" --name B --start 0 --end 2 --bpm 120 --distance 0.05 --lead-ms $L ${EXTRA:-} \
    --samples-dir "$TMP/lead$L/samples" --manifest "$TMP/lead$L/man.txt" --events "$TMP/lead$L/ev.txt" --table "$TMP/lead$L/tab.tsv" > /dev/null || exit 2
done
$PY "$HERE/test_extract.py" lead "$TMP" || fail=1
# pitch: a bass line (--transpose f0), chord stabs at four transpositions and a tremolo pad (--transpose shift), roots spaced
$PY "$HERE/test_extract.py" xmake "$TMP/x/orig" || exit 2
xrun() {   # name, directory suffix, extract options...
  local n=$1 sfx=$2; shift 2
  local D="$TMP/x/x_$n$sfx"
  mkdir -p "$D"
  $PY "$EXTRACT" --stem "$TMP/x/orig/$n.wav" --name P --start 0 --end 12 --bpm 120 --roots spaced "$@" \
    --samples-dir "$D/samples" --manifest "$D/man.txt" --events "$D/ev.txt" --table "$D/tab.tsv" > "$D/extract.log" || { cat "$D/extract.log"; exit 2; }
  $PY "${ASSEMBLE:-$HERE/assemble.py}" --dir "$D" --bpm 120 --bars 6 --part P "$D/ev.txt" "$D/man.txt" other || exit 2
  ( cd "$D" && "$TMP/play" full out 2> render.log ) || { cat "$D/render.log"; exit 2; }
}
xrun bass "" --distance 0.05 --transpose f0
xrun stab "" --distance 0.05 --transpose shift
xrun pad "" --distance 0.05 --transpose shift --merge-sustain 0.05
xrun pad n --distance 0.05 --transpose shift
xrun pad x0 --distance 0
xrun pad x8 --distance 0 --xfade-ms 8
$PY "$HERE/test_extract.py" xcheck "$TMP/x" || fail=1
# sharing only what nulls
$PY "$HERE/test_extract.py" smake "$TMP/x/orig" || exit 2
xrun shares spec --distance 0.1
xrun shares null --share-null -20
xrun shares rise --share-null -20 --no-share-rising
$PY "$HERE/test_extract.py" scheck "$TMP/x" || fail=1
# exact flag
$PY "$HERE/test_extract.py" exactmake "$TMP/exact" || exit 2
for ex in 1 0; do
  ( cd "$TMP/exact/exact$ex" && SOLO=x "$TMP/play" full out 2> render.log ) || { cat "$TMP/exact/exact$ex/render.log"; exit 2; }
done
$PY "$HERE/test_extract.py" exactcheck "$TMP/exact" || fail=1
if [ $fail = 0 ]; then echo "PASS test_extract"; else echo "FAIL test_extract"; fi
exit $fail
