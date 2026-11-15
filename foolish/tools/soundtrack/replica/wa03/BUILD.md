# wa03 recreation, how to rebuild
The notes are `notes_final.tsv` (part, start s, end s, MIDI, velocity); the pipeline that made them is in tools/replica: wa03_nmf.py (fit), wa03_post.py (exact onsets, pedal), wa03_resid.py, wa03_vel.py, wa03_timing.py, wa03_eqloop.py.
Render: `tools/replica/wa03_build.sh notes_final.tsv OUTDIR cello=lyria_vib|lyria|vsco|vsco_vib|none piano=salamander` (SAMPLES=/Users/alex/Dev/soundtrack-samples).
The instrument is a build parameter; HALL_RT, HALL_SEND, CELLO_EQ, PIANO_EQ, MASTER_EQ, PIANO_ATT_MS are environment variables (see replica/wa03_play.c header).
The master EQ default was fitted with the Lyria cello; for VSCO the refit is in the iteration log (iteration 16).
