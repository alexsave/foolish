#!/bin/bash
# flac2wav.sh dir : converts every .flac under dir to a .wav beside it with macOS afconvert (a system tool), then removes the flac
find "$1" -name "*.flac" -print0 | while IFS= read -r -d '' f; do
  w="${f%.flac}.wav"; [ -s "$w" ] || afconvert -f WAVE -d LEI24 "$f" "$w" && rm -f "$f"
done
