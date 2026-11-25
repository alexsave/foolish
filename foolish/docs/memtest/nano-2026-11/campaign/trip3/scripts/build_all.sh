#!/bin/bash
# build_all.sh: trip 3, the four builds M1 M2 M3 M4 serially, each from a clean throwaway detached worktree of foolish
# (removed with git worktree remove when done), through build.sh (BUILD_CPUS=2). Logs to ../out/builds/build_NAME.log.
# Writes ../builds_done.txt when the last build is done.
set -uo pipefail
T=/Users/alex/Dev/foolish-bench-results/campaign/trip3; F=/Users/alex/Dev/foolish
for pair in M1:77fef4bef3dc88a775a5e278dbcfe941059979c5 M2:05cf2501a5cd5105c770f6f2b4e569a2f8298649 M3:59c1150547d6d97f3104d922399d39e4f440cf64 M4:11b34d05402d9a8e0c340e7b796d3d9ba543bfaa; do
  n=${pair%%:*}; sha=${pair#*:}; wt=/Users/alex/Dev/foolish-wt-trip3-$n
  git -C $F worktree add --detach $wt $sha > $T/out/builds/worktree_$n.log 2>&1 || { echo "worktree $n failed"; cat $T/out/builds/worktree_$n.log; exit 1; }
  BUILD_CPUS=2 bash $T/scripts/build.sh $wt $n > $T/out/builds/build_$n.log 2>&1; rc=$?
  tail -4 $T/out/builds/build_$n.log
  git -C $F worktree remove $wt >> $T/out/builds/worktree_$n.log 2>&1 || echo "worktree remove $n failed"
  [ $rc = 0 ] || { echo "build $n failed rc=$rc"; exit 1; }
done
echo "BUILDS DONE $(date -u +%FT%TZ)" > $T/builds_done.txt; cat $T/builds_done.txt
