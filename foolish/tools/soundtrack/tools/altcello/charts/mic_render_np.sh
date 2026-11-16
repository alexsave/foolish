#!/bin/bash
# proximity effect isolated: the same positions with --no-proximity (Bach only)
IN=/Users/alex/Music/foolish_cities_v5/alt_cello/audio/music1/01_bach_s0_dry.wav; EXE=/private/tmp/claude-501/mic1/altcello; R=/private/tmp/claude-501/mic1/raw
for spec in "m1 2.7,3.6952,0.72" "m2 2.7,3.3783,0.7354" "m3 2.7,2.9937,0.8456" "m5 2.7,3.805,1.05"; do
  set -- $spec
  nice -n 15 $EXE room $IN $R/bach_$1np.wav --rt60 0.6 --abs-level --pattern cardioid --mic $2 --no-proximity >/dev/null
done
nice -n 15 $EXE room $IN $R/bach_m4np.wav --rt60 0.6 --abs-level --pattern cardioid --dist 1.5 --no-proximity >/dev/null
