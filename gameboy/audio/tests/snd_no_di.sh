#!/bin/sh
# snd_no_di.sh - the driver never turns interrupts off (DESIGN.md "Tests"):
# it runs in the timer interrupt with them on again (src/input.c), and a
# window it opened would delay the VBlank and every other interrupt. Fails on
# any di, CRITICAL or disable_interrupts in snd.c.
cd "$(dirname "$0")/.." || exit 2
f=${1:-snd.c}
hits=$(grep -nE '\bdi\b|CRITICAL|disable_interrupts|__critical' "$f" | grep -vE '^[0-9]+:[[:space:]]*//')
if [ -n "$hits" ]; then echo "snd_no_di: interrupts turned off in $f:"; echo "$hits"; exit 1; fi
echo "snd_no_di: ok"
