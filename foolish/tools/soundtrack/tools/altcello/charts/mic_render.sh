#!/bin/bash
# renders the dry input through the six mic positions (room 0.6 s, absolute level from the default placement). Usage: render_mics.sh IN.wav PREFIX
IN=$1; P=$2
EXE=/private/tmp/claude-501/mic1/altcello
R=/private/tmp/claude-501/mic1/raw
run() { n=$1; shift; nice -n 15 $EXE room "$IN" $R/${P}_$n.wav --rt60 0.6 --abs-level "$@" | grep -E "^room|DRR" | tr '\n' ' '; echo; }
run m1 --pattern cardioid --mic 2.7,3.6952,0.72
run m2 --pattern cardioid --mic 2.7,3.3783,0.7354
run m3 --pattern cardioid --mic 2.7,2.9937,0.8456
run m4 --pattern cardioid --dist 1.5
run m5 --pattern cardioid --mic 2.7,3.805,1.05
run m6 --pattern omni --mic 2.7,3.6952,0.72
