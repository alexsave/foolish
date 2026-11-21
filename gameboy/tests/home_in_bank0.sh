#!/bin/sh
# The bank loader (gfx.c load_full_banked) runs with ROM bank 2 mapped over
# 0x4000-0x7FFF, so it and everything in _HOME must sit below 0x4000.
line=$(grep -E '^_HOME ' "$1") || { echo "home_in_bank0: no _HOME in $1"; exit 1; }
start=$(echo "$line" | awk '{print $2}')
size=$(echo "$line" | awk '{print $3}')
end=$(( 0x$start + 0x$size ))
if [ "$end" -gt 16384 ]; then echo "home_in_bank0: _HOME ends at $(printf 0x%X $end), past bank 0"; exit 1; fi
echo "home_in_bank0: ok (_HOME 0x$start..$(printf 0x%X $end))"
