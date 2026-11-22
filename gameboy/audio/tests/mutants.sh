#!/bin/sh
# mutants.sh - break one rule of snd.c at a time in a copy; host/snd_test.c
# must fail on each. Run from gameboy/audio (make test).
set -u
CC=${CC:-cc}
mkdir -p build/mut
pass=0; fail=0
try() {
    name=$1; script=$2
    sed "$script" snd.c > build/mut/snd.c
    if cmp -s snd.c build/mut/snd.c; then echo "mutant $name: pattern not found"; fail=$((fail+1)); return; fi
    cp snd.h build/mut/snd.h
    $CC -O2 -std=c11 -w -I build/mut -DSND_HOST host/snd_test.c build/mut/snd.c snd_tab.c -o build/mut/t || { echo "mutant $name: does not build"; fail=$((fail+1)); return; }
    if build/mut/t > build/mut/out.txt; then echo "mutant $name: SURVIVED"; fail=$((fail+1)); else echo "mutant $name: caught ($(grep -m1 FAIL build/mut/out.txt | sed 's/.*: //'))"; pass=$((pass+1)); fi
}
try "strike starts in the call" 's/const uint8_t d = (uint8_t)(delay + 1u + k \* SND_STAGGER);/const uint8_t d = (uint8_t)(delay + k * SND_STAGGER); if (!d) { start(cue, flags); continue; }/'
try "no stagger" 's/delay + 1u + k \* SND_STAGGER/delay + 1u/'
try "cap 7" 's/if (n > SND_MAX_HITS) n = SND_MAX_HITS;/if (n > 7) n = 7;/'
try "no refusal" 's/if (snd_owner != SND_OWNER_SOUND) { snd_refused++; return; }/if (0) { snd_refused++; return; }/'
try "yield keeps the voices" '/^void snd_yield/,/^}/ s/S.v\[i\].row = 0; silence(i);/silence(i);/'
try "yield keeps the queue"  '/^void snd_yield/,/^}/ s/S.q_wait\[i\] = 0;/;/'
try "backwards ignored" 's/while (r\[1\].t) r++;/;/'
try "far ignored" 's/if ((flags \& SND_FAR) \&\& ch != 2)/if (0)/'
echo "mutants: $pass caught, $fail not"
[ $fail -eq 0 ]
