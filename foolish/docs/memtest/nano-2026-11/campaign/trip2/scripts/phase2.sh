#!/bin/bash
# phase2.sh [human]: the three rounds, then the human-pace runs if asked and under 34 box minutes, then push, sync, grav/down.sh
# (unchanged) to out/teardown.txt and leftovers.sh appended to it. The teardown runs whatever the rounds did.
T=/Users/alex/Dev/foolish-bench-results/campaign/trip2; SC=$T/scripts; G=/Users/alex/Dev/foolish-bench-results/grav
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
teardown() {
  [ -f $T/out/teardown_started ] && return; touch $T/out/teardown_started
  bash $SC/trip2.sh push sync
  bash $G/down.sh > $T/out/teardown.txt 2>&1
  echo "terminated_by_utc=$(date -u +%FT%TZ) epoch=$(date +%s)" >> $T/out/launch.txt
  bash $SC/leftovers.sh >> $T/out/teardown.txt 2>&1
  touch $T/out/teardown_done; cat $T/out/teardown.txt
}
trap teardown EXIT
bash $SC/trip2.sh rounds
[ "${1:-}" = human ] && bash $SC/trip2.sh human
echo PHASE2_RUNS_DONE
