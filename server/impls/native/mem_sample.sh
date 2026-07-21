#!/usr/bin/env bash
# mem_sample.sh — sample a running process's /proc/<pid>/status VmRSS at a
# fixed interval for a fixed window, and report idle-vs-mean-vs-peak plus the
# kernel's own VmHWM (high-water mark). Deliverable B of PROFILE_HOTPATH.md's
# "T1c" work: throughput/latency numbers alone don't answer "how much RAM
# does N concurrent games/connections cost" — this does, and doing it by
# periodic sampling (rather than one before/after read) also gives a MEAN
# under load, not just two endpoints.
#
# Usage:
#   mem_sample.sh <pid> <duration_secs> [interval_secs]
#
# Typical use: start the server, note its pid, start a load run in the
# background, then sample for (about) the load run's duration:
#   ./foolish_server 8099 &  SRV=$!
#   ./foolish_hammer --port=8099 --games=40 --seats=4 --secs=10 --mode=ws &
#   ./mem_sample.sh $SRV 10 0.2 > /tmp/mem_40games.csv
#
# Output: a CSV time series (t_secs,vmrss_kb) on stdout — redirect it to a
# file to keep the raw samples — and a summary block on stderr so it is
# visible even when stdout is redirected.
set -uo pipefail

PID="${1:?usage: mem_sample.sh <pid> <duration_secs> [interval_secs]}"
DUR="${2:?usage: mem_sample.sh <pid> <duration_secs> [interval_secs]}"
INTERVAL="${3:-0.2}"

STATUS="/proc/$PID/status"
[ -r "$STATUS" ] || { echo "mem_sample.sh: no such pid $PID (or /proc not readable)" >&2; exit 1; }

vmrss_kb() { awk '/^VmRSS:/{print $2; exit}' "$STATUS" 2>/dev/null; }
vmhwm_kb() { awk '/^VmHWM:/{print $2; exit}' "$STATUS" 2>/dev/null; }

echo "t_secs,vmrss_kb"

SUM=0
N=0
PEAK=0
FIRST=""
T0=$(date +%s.%N)
while :; do
    NOW=$(date +%s.%N)
    ELAPSED=$(awk -v a="$NOW" -v b="$T0" 'BEGIN{printf "%.3f", a-b}')
    if awk -v e="$ELAPSED" -v d="$DUR" 'BEGIN{exit !(e>=d)}'; then break; fi
    RSS=$(vmrss_kb)
    [ -n "$RSS" ] || break   # process exited mid-sample — stop, don't fail the run
    [ -z "$FIRST" ] && FIRST="$RSS"
    echo "$ELAPSED,$RSS"
    SUM=$((SUM + RSS))
    N=$((N + 1))
    [ "$RSS" -gt "$PEAK" ] && PEAK=$RSS
    sleep "$INTERVAL"
done

if [ "$N" -eq 0 ]; then
    echo "mem_sample.sh: got zero samples (process exited immediately?)" >&2
    exit 1
fi

MEAN=$(awk -v s="$SUM" -v n="$N" 'BEGIN{printf "%.1f", s/n}')
HWM=$(vmhwm_kb)

{
    echo "mem_sample: pid=$PID samples=$N window=${DUR}s interval=${INTERVAL}s"
    echo "  first sample (~idle-at-start): ${FIRST} kB"
    echo "  mean (under load):             ${MEAN} kB"
    echo "  peak (sampled):                ${PEAK} kB"
    echo "  VmHWM (kernel watermark):      ${HWM:-n/a} kB   (can exceed 'peak (sampled)' — catches spikes between samples)"
} >&2
