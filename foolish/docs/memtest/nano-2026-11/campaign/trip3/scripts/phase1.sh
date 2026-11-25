#!/bin/bash
# phase1.sh: trip 3 (copy of trip 2's): launch (up_trip3.sh), wait for both boxes, check, the hand install of perf, bpftrace
# and strace, check again, the symbol and bpftrace check on cbase_M3, and the throwaway warm-up run wD. Stops there so the
# warm-up can be inspected before the rounds (phase2.sh). The watchdog is started separately, before this script.
T=/Users/alex/Dev/foolish-bench-results/campaign/trip3; SC=$T/scripts; G=/Users/alex/Dev/foolish-bench-results/grav
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
bash $SC/up_trip3.sh > $T/out/up.txt 2>&1 || { echo "UP FAILED"; cat $T/out/up.txt; exit 1; }
cat $T/out/up.txt
bash $SC/waitready.sh || { echo "WAITREADY FAILED"; exit 1; }
bash $SC/trip3.sh check install check symcheck warm
echo PHASE1_DONE
