#!/bin/sh
# The endgame solver's cross-compiler check: the ROM must solve as the clang
# build does; then its times on the Game Boy's scanline clock.
cd "$(dirname "$0")/.." || exit 2
want=$(build/solvecheck_host) || exit 1
echo "clang: $want"
printf '@1 until _sc_done 01\n+0 expect _sc_sum %s\n+0 peek _sc_resolved 2\n+0 peek _sc_nodes 4\n+0 peek _sc_lines 4\n+0 peek _sc_lines_max 4\n+0 peek _sc_nodes_at_max 4\n' "$want" > build/solvecheck_script.txt
build/gbrun build/solvecheck.gb build/solvecheck_script.txt build/solvecheck.noi
