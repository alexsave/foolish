#!/usr/bin/env bash
# Loss analysis for paired A/B runs.
#
#   1) Run a paired eval with --dump to collect per-seed outcomes:
#        ./build/cnitro_eval --strategy=semtex --control=cordite --opp=cordite \
#            --players=4 --games=300 --seed-start=910001 --dump=/tmp/pairs.txt
#   2) List regression seeds (hero finished WORSE than control):
#        ./analyze_losses.sh seeds /tmp/pairs.txt
#   3) Diff one game move-by-move (first divergent decision + context):
#        ./analyze_losses.sh diff <seed> <pc> [hero] [control] [opp]
#
# The traces are deterministic per seed, so the first differing line is the
# first decision where the two bots disagree; everything before it is shared.
set -euo pipefail
cd "$(dirname "$0")"

cmd=${1:?usage: analyze_losses.sh seeds|diff ...}

if [ "$cmd" = seeds ]; then
    dump=${2:?dump file}
    awk '$3 > $4 { print "pc=" $1 " seed=" $2 "  hero=" $3 " ctrl=" $4 " (delta +" $3-$4 ")" }' "$dump"
    exit 0
fi

if [ "$cmd" = diff ]; then
    seed=${2:?seed}
    pc=${3:?player count}
    hero=${4:-semtex}
    ctrl=${5:-cordite}
    opp=${6:-cordite}
    a=$(mktemp) ; b=$(mktemp)
    ./build/cnitro_eval --strategy="$hero" --opp="$opp" --players="$pc" --inspect="$seed" > "$a" 2>/dev/null
    ./build/cnitro_eval --strategy="$ctrl" --opp="$opp" --players="$pc" --inspect="$seed" > "$b" 2>/dev/null
    echo "=== deal (shared) ==="
    sed -n 2p "$a"
    div=$(diff <(cat "$a") <(cat "$b") | head -1 | sed 's/[acd].*//')
    if [ -z "$div" ]; then echo "traces identical"; rm -f "$a" "$b"; exit 0; fi
    echo "=== first divergence at line $div (context: 6 before, 12 after) ==="
    echo "--- $hero ---"
    sed -n "$((div>8 ? div-6 : 2)),$((div+12))p" "$a"
    echo "--- $ctrl ---"
    sed -n "$((div>8 ? div-6 : 2)),$((div+12))p" "$b"
    echo "=== outcomes ==="
    tail -2 "$a" | head -2 | sed "s/^/$hero: /"
    tail -2 "$b" | head -2 | sed "s/^/$ctrl: /"
    rm -f "$a" "$b"
    exit 0
fi

echo "unknown command: $cmd" >&2
exit 1
