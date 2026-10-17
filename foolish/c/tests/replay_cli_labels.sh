#!/bin/bash
# cnitro_replay prints every card through card_text.h's labels. A card value is a
# rank INDEX (1 is the two, 5 the six, ACE_VALUE the ace), and a table of its own
# once printed each card one rank off. 2..5 players deal the 36-card deck, so the
# only ranks a printed card may carry are 6 to A, and an ace is always dealt.
set -euo pipefail
bin=${1:-build/cnitro_replay}
labels=$(for seed in $(seq 1 20); do
    "$bin" --seed="$seed" --players=2 --pairs=rand | sed -n 's/^  p[0-9] ([a-z]*): //p'
done | tr ' ' '\n' | sed 's/[^0-9JQKA].*$//' | sort -u | tr '\n' ' ')
want="10 6 7 8 9 A J K Q "
if [ "$labels" != "$want" ]; then
    echo "replay_cli_labels: dealt ranks printed as [$labels], want [$want]" >&2
    exit 1
fi
echo "replay_cli_labels: ok"
