#!/bin/bash
# render_vib_demo.sh SUFFIX TAG : renders the on-then-off demo (score_vib.txt) with the sample sets lyria_<inst>SUFFIX.txt
# (SUFFIX _flat = v5, _flat_v6 = v6) and the engine vibrato of each instrument (cello 10.1 c 5.35 Hz, haegeum 11.9 c 5.5 Hz,
# trumpet 5.5 c 4.7 Hz), into lyria_instruments_demo/out/full_<TAG>.wav and sections_<TAG>.txt. Niced.
cd "$(dirname "$0")/../../lyria_instruments_demo" || exit 1
S=$1; T=$2
./run_render.sh 0.5 -DSCORE='"score_vib.txt"' -DCELLO_SUF="\"$S\"" -DHAEGEUM_SUF="\"$S\"" -DTRUMPET_SUF="\"$S\"" \
  -DCELLO_VIBC=10.1 -DCELLO_VIBHZ=5.35 -DHAEGEUM_VIBC=11.9 -DHAEGEUM_VIBHZ=5.5 -DTRUMPET_VIBC=5.5 -DTRUMPET_VIBHZ=4.7 | tail -2
cp out/full.wav out/full_$T.wav; cp out/sections.txt out/sections_$T.txt
