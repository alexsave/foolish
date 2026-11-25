#!/bin/bash
# phase2.sh: trip 3 (copy of trip 2's): the three rounds and the count and perf runs (trip3.sh rounds), the perf
# post-processing on the load box (proc), then push, sync, grav/down.sh (unchanged) to out/teardown.txt and leftovers.sh
# appended to it. The teardown runs whatever the rounds did (trap EXIT). With NORUNS=1 it only tears down.
T=/Users/alex/Dev/foolish-bench-results/campaign/trip3; SC=$T/scripts; G=/Users/alex/Dev/foolish-bench-results/grav
export AWS_PAGER= AWS_DEFAULT_REGION=us-east-1
teardown() {
  [ -f $T/out/teardown_started ] && return; touch $T/out/teardown_started
  [ -z "${NORUNS:-}" ] && bash $SC/trip3.sh proc
  bash $SC/trip3.sh push sync
  bash $G/down.sh > $T/out/teardown.txt 2>&1
  echo "terminated_by_utc=$(date -u +%FT%TZ) epoch=$(date +%s)" >> $T/out/launch.txt
  bash $SC/leftovers.sh >> $T/out/teardown.txt 2>&1
  touch $T/out/teardown_done; cat $T/out/teardown.txt
}
trap teardown EXIT
[ -z "${NORUNS:-}" ] && bash $SC/trip3.sh rounds
echo PHASE2_RUNS_DONE
