#!/bin/sh
# The live-think handler under stress (make livestress): moscowcheck and
# solvecheck built with LIVE_STRESS, the handler's stand-in running on every
# tick of a timer at 1024 Hz on its own stack in cart RAM bank 3, while Moscow
# and the solver switch ROM and RAM banks under it. Both must still reach the
# clang build's sums: a bank put back wrong mid-search changes them.
cd "$(dirname "$0")/.." || exit 2
LCC=${LCC:-$GBDK/bin/lcc}
S=build/stress
mkdir -p $S
set -e
$LCC -DGD_BANKED_BUILD -DLIVE_STRESS -Ibuild -c -o $S/input.o src/input.c
$LCC -DGD_BANKED_BUILD -DLIVE_STRESS -Ibuild -c -o $S/moscowcheck.o tests/moscowcheck.c
# solvecheck on its first game only (-DSC_GAMES=1, the clang side too): under
# the stress it runs about 11 times slower, past gbrun's wait for three games
$LCC -DGD_BANKED_BUILD -DLIVE_STRESS -DSC_GAMES=1 -Ibuild -c -o $S/solvecheck.o tests/solvecheck.c
${HOSTCC:-clang} -std=c11 -O2 -DSOLVECHECK_HOST -DSC_GAMES=1 -Ibuild -o $S/solvecheck_host tests/solvecheck.c src/gd_rules.c src/gd_menu.c src/gd_rng.c src/gd_botlib.c src/gd_bot_hw.c src/gd_sim.c src/gd_solve.c
common="build/rom/gd_rules.o build/rom/gd_rng.o build/rom/gd_stack.o build/rom/gd_botlib.o $S/input.o build/rom/live.o build/rom/gd_menu.o build/rom/gd_bot_hw.o build/rom/gd_sim.o build/rom/gd_solve.o"
$LCC -Wl-m -Wl-j -Wl-b_HOME=0x0200 -Wl-b_CODE=0x0800 -Wm-yt0x02 -Wm-ya4 -Wm-yo16 -o $S/moscowcheck.gb $common $S/moscowcheck.o build/rom/gd_bot_moscow.o
$LCC -Wl-m -Wl-j -Wl-b_HOME=0x0200 -Wl-b_CODE=0x0800 -Wm-yt0x02 -Wm-ya4 -Wm-yo16 -o $S/solvecheck.gb $common $S/solvecheck.o
set +e
# the stress runs are long: let an until wait a million frames
export GBRUN_UNTIL_MAX=1000000
fail=0
want=$(build/moscowcheck_host 2>/dev/null)
printf '@1 until _sc_done 01\n+0 expect _sc_sum %s\n+0 peek _sc_ticks 4\n' "$want" > $S/m.txt
build/gbrun $S/moscowcheck.gb $S/m.txt $S/moscowcheck.noi | tee $S/m.out
grep -q FAILED $S/m.out && fail=1
want=$($S/solvecheck_host 2>/dev/null)
printf '@1 until _sc_done 01\n+0 expect _sc_sum %s\n+0 peek _sc_ticks 4\n+0 peek _sc_resolved 2\n' "$want" > $S/s.txt
build/gbrun $S/solvecheck.gb $S/s.txt $S/solvecheck.noi | tee $S/s.out
grep -q FAILED $S/s.out && fail=1
[ $fail -eq 0 ] && echo "livestress: ok" || echo "livestress: FAILED"
exit $fail
