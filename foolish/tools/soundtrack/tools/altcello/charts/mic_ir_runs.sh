#!/bin/bash
EXE=/private/tmp/claude-501/mic1/altcello; R=/private/tmp/claude-501/mic1/raw; I=/private/tmp/claude-501/mic1/impulse.wav
for spec in "m1 cardioid 2.7,3.6952,0.72" "m2 cardioid 2.7,3.3783,0.7354" "m3 cardioid 2.7,2.9937,0.8456" "m4 cardioid -" "m5 cardioid 2.7,3.805,1.05" "m6 omni 2.7,3.6952,0.72"; do
  set -- $spec
  if [ "$3" = "-" ]; then m="--dist 1.5"; else m="--mic $3"; fi
  nice -n 15 $EXE room $I $R/irnp_$1.wav --rt60 0.6 --pattern $2 $m --no-proximity --no-noise >/dev/null
  nice -n 15 $EXE room /private/tmp/claude-501/mic1/silence.wav $R/noise_$1.wav --rt60 0.6 --abs-level --pattern $2 $m >/dev/null
done
