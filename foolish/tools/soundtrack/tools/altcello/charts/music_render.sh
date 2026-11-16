#!/bin/bash
# Renders the music1 set with the current default (expert) model, one process at a time. Uses a COPY of the binary taken before the run.
# Raw (unnormalised-to-LUFS) renders go to $RAW; charts/music_post.py level-matches them to -20 LUFS into audio/music1/.
SC=/Users/alex/Music/foolish_cities_v5/alt_cello/scores
RAW=/private/tmp/claude-501/m1/raw
EXE=/private/tmp/claude-501/m1/altcello
FE=/Users/alex/Music/foolish_cities_v5/alt_cello/body_fem/out/hf_body.bodymodes.txt
cd /Users/alex/Dev/foolish-wt-altcello/foolish/tools/soundtrack/tools/altcello || exit 1
mkdir -p $RAW
r() { # name score seed room [extra...]
  n=$1; s=$2; seed=$3; room=$4; shift 4
  nice -n 15 $EXE render $SC/$s $RAW/$n.wav --seed $seed --room $room --trace $RAW/$n "$@" | tee -a $RAW/log.txt
}
: > $RAW/log.txt
for seed in 0 1 2; do
  for room in off 0.6; do r bach_s${seed}_$room bach_bwv1007_bars1_4.txt $seed $room; done
done
r air_s0_off bach_air_g_major.txt 0 off
r air_s0_0.6 bach_air_g_major.txt 0 0.6
r bachslur_s0_off bach_bwv1007_bars1_4.txt 0 off --slur 1
r bachslur_s0_0.6 bach_bwv1007_bars1_4.txt 0 0.6 --slur 1
export ALTCELLO_KNOBS="fe=2,fe_path=$FE"
r bachfe_s0_off bach_bwv1007_bars1_4.txt 0 off
r bachfe_s0_0.6 bach_bwv1007_bars1_4.txt 0 0.6
echo DONE >> $RAW/log.txt
