#!/bin/sh
# Seoul's cross-compiler check: the ROM must make the clang build's decisions.
cd "$(dirname "$0")/.." || exit 2
want=$(build/seoulcheck_host) || exit 1
echo "clang: $want"
printf '@1 until _sc_done 01\n+0 expect _sc_sum %s\n' "$want" > build/seoulcheck_script.txt
build/gbrun build/seoulcheck.gb build/seoulcheck_script.txt build/seoulcheck.noi
