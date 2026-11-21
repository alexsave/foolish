#!/bin/bash
# Mutation check of the differential test: break one rule at a time in a COPY of
# src/ (build/mut/src), rebuild the difftest against it, and require it to FAIL.
# A mutant that passes is a rule the test does not prove. The sources in src/
# are never touched.
#
# The mutants are tests/mutants.txt, one per line.
set -u
cd "$(dirname "$0")/.." || exit 2
GAMES=${GAMES:-1000}
FOOLISH=../foolish/c/src
SHARED=../shared/c
CC=${HOSTCC:-clang}
FLAGS="-std=c11 -O2 -w -DMAX_LEGAL_MOVES=65536 -DMAX_MOVE_CARDS=40 -DMAX_LOGS=64"


caught=0; missed=0; i=0
while IFS='~' read -r file from to what; do
    [ -z "$file" ] && continue
    i=$((i+1))
    d=build/mut/$i
    rm -rf "$d"; mkdir -p "$d/src"
    cp src/*.c src/*.h "$d/src/"
    FROM="$from" TO="$to" perl -0pi -e 'my $n = s/\Q$ENV{FROM}\E/$ENV{TO}/g; die "no match\n" unless $n' "$d/src/$file" \
        || { echo "mutant $i: TEXT NOT FOUND ($what)"; missed=$((missed+1)); continue; }
    # the portable core (the Makefile's CORE), from the mutated copy
    core=""
    for c in $(make -s -f Makefile print-core); do core="$core $d/$c"; done
    if ! $CC $FLAGS -Ibuild -I$FOOLISH -o "$d/difftest" tests/difftest.c $core \
            build/hw_prod.o $FOOLISH/game.c $FOOLISH/legal.c $FOOLISH/card.c $SHARED/deal_rng.c 2>"$d/cc.log"; then
        echo "mutant $i: DOES NOT COMPILE ($what)"; missed=$((missed+1)); continue
    fi
    # the solver's mutants are proven by the solver test, the rest of the sim's
    # by the sim test, the rest by the difftest
    if [ "${what#solve:}" != "$what" ]; then
        $CC -std=c11 -O2 -w -DMAX_LOGS=64 -Ibuild -I$FOOLISH -o "$d/solvetest" tests/solvetest.c $core \
            $FOOLISH/game.c $FOOLISH/legal.c $FOOLISH/card.c $SHARED/deal_rng.c \
            build/cordite_sim.o 2>>"$d/cc.log" || { echo "mutant $i: SOLVETEST DOES NOT COMPILE ($what)"; missed=$((missed+1)); continue; }
        out=$("$d/solvetest" "${SOLVE_GAMES:-60}" 1 20000 2>&1)
    elif [ "$file" = gd_sim.c ]; then
        $CC -std=c11 -O2 -w -DMAX_LOGS=64 -Ibuild -I$FOOLISH -o "$d/simtest" tests/simtest.c "$d/src/gd_rules.c" "$d/src/gd_menu.c" \
            "$d/src/gd_rng.c" "$d/src/gd_botlib.c" "$d/src/gd_sim.c" $FOOLISH/game.c $FOOLISH/legal.c $FOOLISH/card.c $SHARED/deal_rng.c \
            build/cordite_sim.o 2>>"$d/cc.log" || { echo "mutant $i: SIMTEST DOES NOT COMPILE ($what)"; missed=$((missed+1)); continue; }
        out=$("$d/simtest" "$GAMES" 1 2>&1)
    else
        out=$("$d/difftest" "$GAMES" 1 2>&1)
    fi
    if [ $? -ne 0 ]; then
        echo "mutant $i: caught - $what"
        echo "$out" | grep -m1 MISMATCH | sed 's/^/    /'
        caught=$((caught+1))
    else
        echo "mutant $i: MISSED - $what"; missed=$((missed+1))
    fi
done < <(grep -v "^#" tests/mutants.txt | grep -F -- "${ONLY:-}")
echo "mutants: $caught caught, $missed missed"
[ "$missed" -eq 0 ]
