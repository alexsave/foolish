#!/bin/bash
# phase1.sh: launch (up_trip2.sh), the watchdog, wait for both boxes, check, and the throwaway warm-up run wA. Stops there so
# the warm-up can be inspected before the rounds (phase2.sh).
T=/Users/alex/Dev/foolish-bench-results/campaign/trip2; SC=$T/scripts; G=/Users/alex/Dev/foolish-bench-results/grav
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
bash $SC/up_trip2.sh > $T/out/up.txt 2>&1 || { echo "UP FAILED"; cat $T/out/up.txt; exit 1; }
cat $T/out/up.txt
bash $SC/waitready.sh || { echo "WAITREADY FAILED"; exit 1; }
bash $SC/trip2.sh check warm
echo PHASE1_DONE
