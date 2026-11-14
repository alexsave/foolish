#!/bin/sh
# seeded_repeat.sh <program>...: the seeded-repeat gate (c_base/Makefile UNIT_SEEDED). Each program
# runs twice from c_base/ with T_COUNTS=1 (t.h prints "count <test> <checks>" after every test) and the
# two stdout streams, count lines and summary line, must be byte-identical. On a difference the first
# differing line of each run is named and the exit status is 1.
set -u
mkdir -p build/logs
for p in "$@"; do
    out1=build/logs/$p.rep1
    out2=build/logs/$p.rep2
    T_COUNTS=1 "build/test/$p" > "$out1" 2> "$out1.err"
    T_COUNTS=1 "build/test/$p" > "$out2" 2> "$out2.err"
    if ! cmp -s "$out1" "$out2"; then
        echo "seeded-repeat: $p ran two ways; first differing line:"
        diff "$out1" "$out2" | sed -n '1,4p'
        exit 1
    fi
    echo "seeded-repeat: $p: $(grep -c '^count ' "$out1") tests, two runs the same: $(tail -n 1 "$out1")"
done
