#!/bin/bash
# watchdog.sh: at minute 44 after the server's run-instances, tears the trip down with grav/down.sh unless phase2.sh already
# started its teardown (out/teardown_started). Protects the 45-minute budget if the driver or the session stalls.
T=/Users/alex/Dev/foolish-bench-results/campaign/trip2; G=/Users/alex/Dev/foolish-bench-results/grav
n=0; until [ -f $T/out/launch_epoch ] || [ $n -gt 120 ]; do sleep 5; n=$((n+1)); done
LE=$(cat $T/out/launch_epoch); dl=$((LE + 44*60))
while [ $(date +%s) -lt $dl ]; do [ -f $T/out/teardown_started ] && { echo "teardown started by phase2, watchdog exits $(date -u +%T)"; exit 0; }; sleep 10; done
[ -f $T/out/teardown_started ] && exit 0
touch $T/out/teardown_started; echo "WATCHDOG teardown at $(date -u +%FT%TZ)"
bash $G/down.sh > $T/out/teardown_watchdog.txt 2>&1
echo "terminated_by_watchdog_utc=$(date -u +%FT%TZ)" >> $T/out/launch.txt
bash $T/scripts/leftovers.sh >> $T/out/teardown_watchdog.txt 2>&1; touch $T/out/teardown_done
