#!/usr/bin/env bash
# The chaos suite: every hostile shape the sim can make, run over several
# seeds, reported as one matrix of findings.
#
# A finding here is not automatically a bug. Some are expected by construction
# and the point is the COUNT - view_regression on a datagram link is what an
# unordered transport IS, and a stall behind a griefer is the answer to a
# product question.
#
# Five of them are different, and this script EXITS NON-ZERO on them, which is
# what makes it a gate rather than a wall chart:
#
#   conservation, mutation_on_reject, phantom_hand_loss, seat_mismatch
#       the kernel's own invariants. No wire can excuse one of these; if the
#       transport can make the kernel break a rule, the kernel is broken.
#   queue_overflow
#       not a kernel fault but a RUN fault: a game's request backlog ran out of
#       room, so the run stopped simulating what it says it simulated and every
#       other count in that column is under-reported. A silently wrong answer.
#
# `cross_deal_apply` is deliberately NOT in that set, though an earlier draft of
# this header said it was. It fires for real (ws-hostile, seed 1) and it is a
# finding about the PRODUCT wire, not the kernel: nothing in a frame names which
# deal it was decided on, and the game id survives a rematch, so a move chosen
# just before a re-deal is applied just after it. README.md has it under the
# detectors. It is a bug in the protocol the sim is modelling, and the sim
# reporting it is the sim working.
#
#   bash tools/chaos_suite.sh          # 3 seeds
#   SEEDS="1 2 3 4 5" bash tools/chaos_suite.sh
#
# Exit: 0 if all five gated kinds are zero, 1 otherwise. CI runs it (see
# .github/workflows/foolyard.yml); the printed matrix is for humans.
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

# The gated subset, by NAME - so a reordering of KINDS cannot silently re-point
# the gate at a different finding, the way an index list would.
GATED=(conservation mutation_on_reject phantom_hand_loss seat_mismatch queue_overflow)

is_gated() {
    for g in "${GATED[@]}"; do [ "$g" = "$1" ] && return 0; done
    return 1
}

# Every gated name must really be one of the kinds foolyard prints. A typo here
# ("conservaton") would gate on a count that is always zero because nothing ever
# writes it - a gate that cannot fail, which is the failure this whole file is
# about.
for g in "${GATED[@]}"; do
    found=0
    for k in "${KINDS[@]}"; do [ "$k" = "$g" ] && found=1; done
    [ "$found" = 1 ] || { echo "chaos_suite: gated kind '$g' is not a finding foolyard reports"; exit 2; }
done

printf "%-17s %6s  cons  muta stall phant dupli viewr queue seatm cross latem\n" "config" "games"
printf '%.0s-' {1..110}; printf "\n"

# bash 3.2 (what macOS ships) has no associative arrays, so these are indexed
# in lockstep with KINDS.
NK=${#KINDS[@]}
TOTAL=(); for ((i=0;i<NK;i++)); do TOTAL[$i]=0; done
total_games=0

for cfg in "${CONFIGS[@]}"; do
    name="${cfg%%|*}"; args="${cfg#*|}"
    sum=(); for ((i=0;i<NK;i++)); do sum[$i]=0; done
    games=0
    for sd in $SEEDS; do
        out=$($BIN $args --seed "$sd" 2>&1)
        g=$(echo "$out" | awk '/^  games/{print $4}')
        # An unparsed line is not a zero. If the report's wording moves, every
        # `${v:-0}` below turns into a clean sheet and the gate stops gating -
        # so a missing field is a hard error, not a default.
        [ -n "$g" ] || { echo "chaos_suite: no games line in '$name' seed $sd - the report format moved"; exit 2; }
        games=$((games + g))
        for ((i=0;i<NK;i++)); do
            v=$(echo "$out" | awk -v k="${KINDS[$i]}" '$1==k{print $2}')
            [ -n "$v" ] || { echo "chaos_suite: '$name' seed $sd printed no ${KINDS[$i]} count - the report format moved"; exit 2; }
            sum[$i]=$(( ${sum[$i]} + v ))
            TOTAL[$i]=$(( ${TOTAL[$i]} + v ))
        done
    done
    total_games=$((total_games + games))
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
printf "\n\n"

# The gate. A run that played no games at all would satisfy every count
# trivially, so the total games played is checked too: chaos with nothing in it
# is the same clean sheet as chaos that passed.
failed=""
for ((i=0;i<NK;i++)); do
    if is_gated "${KINDS[$i]}" && [ "${TOTAL[$i]}" != 0 ]; then
        failed="$failed ${KINDS[$i]}=${TOTAL[$i]}"
    fi
done

if [ "$total_games" -lt 100 ]; then
    printf "FAIL: the suite finished only %d games - it is not exercising anything.\n" "$total_games"
    exit 1
fi

if [ -n "$failed" ]; then
    printf "FAIL:%s\n" "$failed"
    printf "These are the kernel's own invariants plus run integrity. No wire\n"
    printf "excuses one: re-run the named config with --deep and a single seed.\n"
    exit 1
fi

printf "PASS: %d games, and all of %s stayed at zero.\n" "$total_games" "${GATED[*]}"
printf "Everything else in the matrix is transport behaviour, not a bug -\n"
printf "see the header of this file and README.md's detector table.\n"
