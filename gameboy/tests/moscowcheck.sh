#!/bin/sh
# Moscow's cross-compiler check: the ROM must make the clang build's decisions.
cd "$(dirname "$0")/.." || exit 2
want=$(build/moscowcheck_host) || exit 1
echo "clang: $want"
printf '@1 until _sc_done 01\n+0 expect _sc_sum %s\n+0 peek _sc_solves 2\n+0 peek _sc_ticks 2\n' "$want" > build/moscowcheck_script.txt
build/gbrun build/moscowcheck.gb build/moscowcheck_script.txt build/moscowcheck.noi
