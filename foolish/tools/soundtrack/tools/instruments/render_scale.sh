#!/bin/bash
# render_scale.sh SUFFIX TAG : renders the C major scale demo (lyria_instruments_demo/score_scale.txt, vibrato off, one 20 s section per
# instrument) with the sample sets lyria_<inst>SUFFIX.txt (_flat = v5, _flat_v6 = v6) into out/full_scale_<TAG>.wav. Niced.
cd "$(dirname "$0")/../../lyria_instruments_demo" || exit 1
S=$1; T=$2
nice clang -O2 -w -DHAE_SEND=0.5 -DSCORE='"score_scale.txt"' -DCELLO_SUF="\"$S\"" -DHAEGEUM_SUF="\"$S\"" -DTRUMPET_SUF="\"$S\"" \
  -DCELLO_VIBC=10.1 -DCELLO_VIBHZ=5.35 -DHAEGEUM_VIBC=11.9 -DHAEGEUM_VIBHZ=5.5 -DTRUMPET_VIBC=5.5 -DTRUMPET_VIBHZ=4.7 -o score_$T score.c || exit 1
mkdir -p out_$T && SAMPLES=/Users/alex/Dev/soundtrack-samples ENGINE=../engine nice ./score_$T full out_$T 2>&1 | tail -2
cp out_$T/full.wav out/full_scale_$T.wav; rm -rf out_$T score_$T
