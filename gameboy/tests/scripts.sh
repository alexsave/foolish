#!/bin/sh
# Every gbrun script in scripts/ against the ROM: each writes its screenshots
# to shots/ and fails on any expect that does not hold.
cd "$(dirname "$0")/.." || exit 2
mkdir -p build/fw
fail=0
for s in scripts/*.txt; do
    out=$(build/gbrun build/durak.gb "$s" build/durak.noi 2>&1)
    rc=$?
    n=$(echo "$out" | grep -c ' ok$')
    if [ $rc -ne 0 ] || echo "$out" | grep -q FAILED; then
        echo "FAIL $s"; echo "$out" | grep -v ' ok$' | grep -v ': shot ' | sed 's/^/    /'; fail=1
    else
        echo "ok   $s ($n expects)"
        echo "$out" | grep ': peek ' | sed 's/^/    /'
    fi
done
${PYTHON:-python3} -I tests/fwcheck.py || fail=1
# tear.txt again on the game with no catch-up between redraws (Makefile
# durak_nocatch.gb): the flush alone must bring the back map up to date
out=$(build/gbrun build/durak_nocatch.gb scripts/tear.txt build/durak_nocatch.noi 2>&1)
if [ $? -ne 0 ] || echo "$out" | grep -q FAILED; then
    echo "FAIL scripts/tear.txt (no catch-up)"; echo "$out" | grep FAILED | sed 's/^/    /'; fail=1
else
    echo "ok   scripts/tear.txt (no catch-up)"
fi
exit $fail
