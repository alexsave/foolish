#!/bin/bash
# Profiles the Bach render (4 bars looped twice, room 0.6) with the macOS sample command. Uses the copy of the binary made for music1.
# usage: charts/profile_music.sh OUTDIR     (OUTDIR/sample.txt, OUTDIR/render.log)
D=${1:-/private/tmp/claude-501/m1/prof}
EXE=/private/tmp/claude-501/m1/altcello
mkdir -p $D
cd /Users/alex/Dev/foolish-wt-altcello/foolish/tools/soundtrack/tools/altcello || exit 1
nice -n 15 $EXE render /private/tmp/claude-501/m1/bach_x2.txt $D/p.wav --seed 0 --room 0.6 > $D/render.log 2>&1 &
pid=$!
sleep 3
/usr/bin/sample $pid 38 1 -file $D/sample.txt > /dev/null 2>&1
wait $pid
cat $D/render.log
