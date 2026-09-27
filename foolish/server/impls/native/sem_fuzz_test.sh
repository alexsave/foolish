#!/usr/bin/env bash
# sem_fuzz_test.sh — the semantic anti-cheat gate. Where fuzz_test.sh proves the
# SERVER is memory-safe against hostile bytes, this proves the KERNEL's legality
# engine (awire_apply) cannot be cheated: it deals real games, plays them out,
# and fires well-formed illegal moves at the engine with full ground truth,
# failing if any of these ever happens (see sem_fuzz.c):
#   - a card not in the acting seat's hand is accepted onto the board (cheat);
#   - an accepted move breaks card conservation (duplication/creation);
#   - a rejected move mutates state (bar the documented PASS_OVERFLOW abort);
#   - a move calculate_legal_moves() enumerated is rejected by awire_apply.
#
# It also runs a NEGATIVE CONTROL — an inverted build that SHOULD report cheats —
# to prove the detector actually fires (a green run from a no-op checker is
# worthless), and an AddressSanitizer/UBSan pass for memory safety of the engine
# under hostile-but-well-formed moves.
set -uo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

GAMES="${1:-4000}"
SEED="${2:-20260724}"
PASS=true

echo "== sem_fuzz_test.sh: games=$GAMES seed=$SEED =="

echo "-- building sem_fuzz + sem_fuzz_asan"
make sem_fuzz sem_fuzz_asan >/dev/null || { echo "FAIL: build"; exit 1; }

# 1) main run — must report zero violations
echo "-- main run"
if ./sem_fuzz "$GAMES" "$SEED"; then
    echo "PASS: no anti-cheat / conservation / consistency violations"
else
    echo "FAIL: sem_fuzz reported violations"; PASS=false
fi

# 2) negative control — an inverted checker MUST find 'cheats', proving the
#    detector isn't a no-op. Built in a scratch copy so the tree is untouched.
echo "-- negative control (inverted anti-cheat check must FAIL)"
TMP="$(mktemp -d /tmp/sem_neg.XXXXXX)"
sed 's/if (!hand_has(pl, a->cards\[i\])) {/if (hand_has(pl, a->cards[i])) {/' sem_fuzz.c > "$TMP/sem_fuzz.c"
KDIR=../../../c
CFLAGS="-O2 -ffast-math -w -I$KDIR/src -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK"
KSRC=$(ls $KDIR/src/*.c | grep -v '/main_')
if cc $CFLAGS "$TMP/sem_fuzz.c" $KSRC -o "$TMP/sem_neg" -lm -lpthread 2>/dev/null; then
    if "$TMP/sem_neg" 200 "$SEED" >/dev/null 2>&1; then
        echo "FAIL: inverted checker reported PASS — the detector is not actually checking"; PASS=false
    else
        echo "PASS: inverted checker correctly reported violations (detector fires)"
    fi
else
    echo "WARN: could not build negative control (skipping)"
fi
rm -rf "$TMP"

# 3) AddressSanitizer + UBSan — engine memory safety under hostile moves
echo "-- AddressSanitizer/UBSan run"
ASAN_ERR="$(mktemp /tmp/sem_asan.XXXXXX.err)"
ASAN_OPTIONS=detect_leaks=0:halt_on_error=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 \
  ./sem_fuzz_asan "$((GAMES/4>0?GAMES/4:1))" "$SEED" >/dev/null 2>"$ASAN_ERR"
if grep -qE "runtime error|ERROR: AddressSanitizer|SUMMARY: (Address|Undefined)" "$ASAN_ERR"; then
    echo "FAIL: sanitizer reported a problem:"; grep -nE "runtime error|AddressSanitizer|SUMMARY:" "$ASAN_ERR" | head; PASS=false
else
    echo "PASS: legality engine clean under AddressSanitizer + UBSan"
fi
rm -f "$ASAN_ERR"

echo
if $PASS; then
    echo "=== sem_fuzz_test.sh: PASS — the legality engine resists cheating and is memory-safe ==="
    exit 0
else
    echo "=== sem_fuzz_test.sh: FAIL — see above ==="
    exit 1
fi
