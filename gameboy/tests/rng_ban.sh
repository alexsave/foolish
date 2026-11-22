#!/bin/sh
# The one random source is src/gd_rng.c. Fail on any other generator or time
# based seed anywhere in the game's sources, tests and tools.
cd "$(dirname "$0")/.." || exit 2
hits=$(grep -nE '\b(rand|random|srand|srandom|arc4random[a-z_]*|drand48|lrand48|time|clock|gettimeofday)[[:space:]]*\(' \
        src/*.c src/*.h tests/*.c tools/*.c audio/*.c audio/*.h audio/host/*.c 2>/dev/null | grep -v '^src/gd_rng\.')
if [ -n "$hits" ]; then
    echo "rng_ban: a stray random or time source:"
    echo "$hits"
    exit 1
fi
echo "rng_ban: ok (one random source: src/gd_rng.c)"
