#!/usr/bin/env bash
# The chaos suite: every hostile shape the sim can make, run over several
# seeds, reported as one matrix of findings.
#
# A finding here is not automatically a bug. Some are expected by construction
# and the point is the COUNT - view_regression on a datagram link is what an
# unordered transport IS, and a stall behind a griefer is the answer to a
# product question. What matters is that the kernel-level ones (conservation,
# mutation_on_reject, phantom_hand_loss, seat_mismatch, cross_deal_apply) stay
# at zero, and that nothing new appears where it did not before.
#
#   bash tools/chaos_suite.sh          # 3 seeds
#   SEEDS="1 2 3 4 5" bash tools/chaos_suite.sh
set -u
cd "$(dirname "$0")/.."

BIN=./foolyard
SEEDS="${SEEDS:-1 2 3}"
[ -x "$BIN" ] || { echo "build first: make"; exit 1; }

# name | args
CONFIGS=(
"quiet-baseline|--games 32 --secs 900 --lineup wellbehaved@400,wellbehaved@600,handwritten@300,random@200"
"ws-hostile|--games 24 --secs 900 --loss 4 --dup 5 --jitter 400 --lineup wellbehaved@200,laggy@600,reconnect@200,handwritten@250"
"datagram-storm|--games 24 --secs 900 --loss 5 --dup 6 --jitter 700 --lineup datagram@150,datagram@450,resender@250,random@120"
"datagram-noloss|--games 24 --secs 900 --loss 0 --dup 4 --jitter 700 --lineup datagram@150,datagram@450,wellbehaved@250,random@120"
"reconnect-storm|--games 24 --secs 900 --loss 8 --jitter 500 --lineup reconnect@200,reconnect@400,wellbehaved@300,handwritten@250"
"resend-storm|--games 24 --secs 900 --dup 10 --jitter 600 --lineup resender@200,resender@500,wellbehaved@300,random@150"
"stale-actors|--games 24 --secs 900 --jitter 300 --lineup stale@1500,stale@2500,wellbehaved@300,handwritten@250"
"poller-mix|--games 24 --secs 900 --jitter 300 --lineup poller@400,wellbehaved@300,handwritten@250,random@200"
"griefer|--games 16 --secs 900 --lineup wellbehaved@300,griefer@0,handwritten@250,random@200"
"slow-server|--games 16 --secs 900 --service-us 40000 --hiccup-pct 25 --hiccup-ms 2000 --jitter 400 --lineup wellbehaved@200,wellbehaved@300,random@100,random@100"
"rematch-churn|--games 24 --secs 900 --jitter 400 --dup 5 --lineup wellbehaved@150,datagram@200,random@80,random@80"
"eight-seat-max|--games 12 --secs 900 --loss 5 --dup 6 --jitter 700 --hiccup-pct 12 --hiccup-ms 900 --lineup datagram@120,datagram@400,resender@200,stale@700,reconnect@150,laggy@600,random@80,handwritten@200"
"deep-checked|--games 12 --secs 900 --deep --loss 4 --dup 6 --jitter 600 --lineup datagram@150,resender@250,stale@800,wellbehaved@200,random@120,handwritten@200"
)

KINDS=(conservation mutation_on_reject stall phantom_hand_loss duplicate_applied
       view_regression queue_overflow seat_mismatch cross_deal_apply move_applied_late)

printf "%-17s %6s  cons  muta stall phant dupli viewr queue seatm cross latem\n" "config" "games"
printf '%.0s-' {1..110}; printf "\n"

# bash 3.2 (what macOS ships) has no associative arrays, so these are indexed
# in lockstep with KINDS.
NK=${#KINDS[@]}
TOTAL=(); for ((i=0;i<NK;i++)); do TOTAL[$i]=0; done

for cfg in "${CONFIGS[@]}"; do
    name="${cfg%%|*}"; args="${cfg#*|}"
    sum=(); for ((i=0;i<NK;i++)); do sum[$i]=0; done
    games=0
    for sd in $SEEDS; do
        out=$($BIN $args --seed "$sd" 2>&1)
        g=$(echo "$out" | awk '/^  games/{print $4}')
        games=$((games + ${g:-0}))
        for ((i=0;i<NK;i++)); do
            v=$(echo "$out" | awk -v k="${KINDS[$i]}" '$1==k{print $2}')
            sum[$i]=$(( ${sum[$i]} + ${v:-0} ))
            TOTAL[$i]=$(( ${TOTAL[$i]} + ${v:-0} ))
        done
    done
    printf "%-17s %6d" "$name" "$games"
    for ((i=0;i<NK;i++)); do
        if [ "${sum[$i]}" = 0 ]; then printf " %5s" "."; else printf " %5d" "${sum[$i]}"; fi
    done
    printf "\n"
done

printf '%.0s-' {1..110}; printf "\n"
printf "%-17s %6s" "TOTAL" ""
for ((i=0;i<NK;i++)); do
    if [ "${TOTAL[$i]}" = 0 ]; then printf " %5s" "."; else printf " %5d" "${TOTAL[$i]}"; fi
done
printf "\n\nKernel-level findings must be zero: conservation, mutation_on_reject,\n"
printf "phantom_hand_loss, seat_mismatch. Anything else is transport behaviour.\n"
