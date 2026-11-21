#!/bin/sh
# The cross-compiler check: the ROM build must produce the clang build's sum.
cd "$(dirname "$0")/.." || exit 2
want=$(build/selfplay_host) || exit 1
printf '@20000 expect _selfplay_done 01\n+0 expect _selfplay_out %s\n' "$want" > build/selfplay_script.txt
echo "clang: $want"
build/gbrun build/selfplay.gb build/selfplay_script.txt build/selfplay.noi
