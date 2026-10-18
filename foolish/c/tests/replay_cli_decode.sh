#!/bin/bash
# cnitro_replay --code reads a share link back: the header, then every event of
# the decoded stream, numbered. The code is the shipped tutorial's (read out of
# src/components/tutorialGame.ts, so nothing here is a frozen copy of it), and a
# retired format must be refused with a non-zero exit, not printed as a game.
set -euo pipefail
bin=${1:-build/cnitro_replay}
code=$(sed -n "s/^export const TUTORIAL_MOVES_CODE = '\([A-Z2-7]*\)'.*/\1/p" ../src/components/tutorialGame.ts)
[ -n "$code" ] || { echo "replay_cli_decode: no tutorial code found" >&2; exit 1; }
out=$("$bin" --code="https://foolish.cards/$code")
total=$(printf '%s\n' "$out" | sed -n 's/^total events: //p')
events=$(printf '%s\n' "$out" | grep -c '^# *[0-9]')
first=$(printf '%s\n' "$out" | grep -m1 '^# *[0-9]' | awk '{print $3}')
if ! printf '%s\n' "$out" | grep -q 'format 10$' || [ -z "$total" ] || [ "$events" != "$total" ] || [ "$first" != game_start ]; then
    echo "replay_cli_decode: header or stream wrong (total=$total events=$events first=$first)" >&2
    printf '%s\n' "$out" | head -8 >&2
    exit 1
fi
if "$bin" --code="A4" >/dev/null 2>&1; then
    echo "replay_cli_decode: a retired (version 7) code was printed as a game" >&2
    exit 1
fi
echo "replay_cli_decode: ok ($total events)"
