#!/bin/sh
# Moscow's cross-compiler check: the ROM must make the clang build's decisions.
# It also reads the stack guard (src/gd_stack.c): this ROM's data ends lower than
# the game's, so its gap is bigger; the check asks that the same stack reach
# would still leave 48 bytes untouched in the game's own gap.
cd "$(dirname "$0")/.." || exit 2
want=$(build/moscowcheck_host) || exit 1
echo "clang: $want"
end() { printf '%d' "$(awk '$2 == "s__HEAP_END" { print $3 }' "$1")"; }
game_end=$(end build/durak.noi); mc_end=$(end build/moscowcheck.noi)
need=$((48 + game_end - mc_end))
echo "stack guard: the game's data ends at $game_end, this ROM's at $mc_end; at least $need untouched bytes here are 48 in the game"
printf '@1 until _sc_done 01\n+0 expect _sc_sum %s\n+0 peek _sc_solves 2\n+0 peek _sc_ticks 2\n+0 peek _gd_stack_free 2\n+0 atleast _gd_stack_free 2 %s\n' "$want" "$need" > build/moscowcheck_script.txt
build/gbrun build/moscowcheck.gb build/moscowcheck_script.txt build/moscowcheck.noi
