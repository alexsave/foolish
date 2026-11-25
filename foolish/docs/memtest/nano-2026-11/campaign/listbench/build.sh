#!/bin/bash
# Builds the bench binaries: A, Ac (counters, touch loop), A0 (no relink) on the A tree; B on the B tree.
set -euo pipefail
LB=/Users/alex/Dev/foolish-bench-results/campaign/listbench
OUT=$LB/out
mk() { nice -n 10 make -j2 -s -C "$1/c_base" -f $LB/listbench.mk LB_OUT=$OUT LB_NAME="$2" LB_DEFS="$3" "$OUT/listbench_$2"; }
mk /Users/alex/Dev/foolish-wt-lists-A A ""
mk /Users/alex/Dev/foolish-wt-lists-A Ac "-DLB_COUNT -DLB_TOUCH"
mk /Users/alex/Dev/foolish-wt-lists-A A0 ""
mk /Users/alex/Dev/foolish-wt-lists-B B "-DLB_B"
ls -la $OUT
