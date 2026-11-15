#!/bin/bash
# T7: no random or time-seeded source anywhere except rng.c / rng.h.
cd "$(dirname "$0")" || exit 2
pat='(^|[^A-Za-z0-9_])(rand|srand|random|srandom|rand_r|arc4random[A-Za-z0-9_]*|drand48|lrand48|mrand48|erand48|getentropy|getrandom|time)[[:space:]]*\(|/dev/u?random|Math\.random|Date\.now'
files=$(ls *.c *.h 2>/dev/null | grep -v -e '^rng\.c$' -e '^rng\.h$')
bad=$(grep -n -E "$pat" $files)
if [ -n "$bad" ]; then
    echo "FAIL test_rng_ban: banned random/time symbol outside rng.c/rng.h:"
    echo "$bad"
    exit 1
fi
echo "PASS test_rng_ban: no banned random symbols in $(echo $files | wc -w | tr -d ' ') files"
