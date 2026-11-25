#!/bin/bash
# Interleaved runs of A (lists), B (pre-496), A0 (A without the residency relink); load recorded.
set -euo pipefail
LB=/Users/alex/Dev/foolish-bench-results/campaign/listbench
cd $LB
OUT=$LB/results.txt
: > $OUT
for round in 1 2 3 4 5 6 7; do
  for v in A B A0; do
    rm -rf $LB/run_$v
    echo "LOAD round $round $v $(uptime | sed 's/.*load averages*: //')" >> $OUT
    nice -n 10 $LB/out/listbench_$v 40 $LB/run_$v 2>/dev/null >> $OUT
  done
done
rm -rf $LB/run_A $LB/run_B $LB/run_A0 $LB/run_try
