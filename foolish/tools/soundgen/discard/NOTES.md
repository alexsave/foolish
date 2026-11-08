# Discard and pickup (2026-11-07, first version from recordings; not yet heard by Alex)

The first try (`../sweep`, layered pass shfffs) "just sound like a single card".
Alex then recorded the real thing (IMG_3046, IMG_3047, IMG_3048) and said discard has "scrapes", then a flip, then "a small soft placement of cards into the discard pile".

## What the recordings show (`an/cut.py`, `an/disc_an.py`)
1. **Scrapes:** the hand sweeps the cards together in 4-5 strokes for about 10 cards.
   - Strokes last 250-1230 ms (median 470) and are 50-200 ms apart.
   - Much fuller than a single card's pass: the body peaks near 300 Hz and is only 11 dB down at 1.5 kHz; a pass is 10 dB thinner below 1 kHz.
   - Rough: the 5 ms level wanders about 2.3 dB around its 50 ms average, with about 6 card-edge clicks per stroke.
   - The lows under 250 Hz are only 6-8 dB over the room, so the room's power is subtracted from every spectrum.
2. **Lift:** the pile is picked up and turned over: a sharp knock, a second softer one 35-62 ms later, a short rustle. 20 dB over the scrapes at peak.
3. **Placement:** the pile set face down on the discard pile: soft and low (150 Hz about 25 dB over 2 kHz), a second touch 15-75 ms later, a few sharp settling clicks. 14 dB under the lift.

## The synth (`an/disc_syn.py`)
- Every part is noise in its measured spectrum, split at 1.5 kHz, each band on its measured envelope, plus strikes for the knocks and clicks, then a 3-pass band fit (`an/*_fit.npy`, rebuilt with `fit`).
- **Scrape:** inside the 14 reps on every measure except crest (14.1 against a maximum of 13.8).
- **Lift and placement:** only 2 reps each, so their ranges are very narrow.
  - The lift is a little short (96 ms over -20 dB against 104-129).
  - The placement's clicks are less sharp (1-4 kHz kurtosis 12 against 18-22).
- Like every other model, the synth keeps the phone's dark band at 10-13 kHz.

## Pickup, from its own recordings (IMG_3052-3054)
Alex on the first version: the lift was too loud, and "there should just be one tap into the hand, and it should be quieter".
- The recordings show no taps: after the scraping, the pile leaves the table with one soft knock, then 0.55-1.2 s later the cards go into the fanned hand as one slide (350-750 ms, brighter than a scrape, swelling to its end).
- Measured by 50 ms RMS against the strokes: the lift -9 to -13 dB, the slide into the hand -2 to -12 dB (median -11).
- The phone was in the hand that takes the cards, so the lows under 280 Hz are handling rumble: they are held at the 301 Hz level and not fitted.
- The discard's lift was 17 dB over the strokes, set from peaks; its three videos measure -3, +11 and +23 dB by 50 ms RMS, so it is now +5 dB.

## "Like digging into the earth with a shovel" (Alex, 2026-11-07)
- **Texture:** the first stroke's level wandered on an 8 ms grain, which measured 4 dB too much flutter at 30-100 Hz in the highs (grit) and 5-7 dB too little below 30 Hz (the hand's slow surges), mostly in the lows.
  Now a 40 ms wander shared by both bands, a little 8 ms grain on the highs, and the lows' own 15 ms wander: within about 2 dB of the reps in every modulation band.
- **Level:** auditioning the strokes raised to a common level played them about 20 dB over the video. In the game they were also 3 dB over a pass; the recordings put them 6.5 dB under one, so they dropped 3.5 dB.
- `SCR_LO_CUT` takes dB off the strokes below 1 kHz, for auditioning only; it is 0 (as recorded).

## At game pace (`an/disc_game.py`)
- One stroke per ~2.5 cards (1-5), each 0.8x a recorded stroke, 60-200 ms apart, a little louder as the pile grows.
- The lift 100-180 ms after the last stroke; the placement after a 500 ms flight (ANIM_TIME_MS).
- Pickup: the same strokes, the soft lift, then after the 500-600 ms flight the one slide into the hand.
- Levels are set by 50 ms RMS, the strokes at g-30.5 (6.5 dB under a pass, as recorded).

## Reproduce
```
python3 -I an/cut.py DIR PDIR   # d3046-d3048.wav and p3052-p3054.wav, mono 48 kHz from the videos
python3 -I an/disc_an.py
mkdir -p synth && python3 -I an/disc_syn.py d6 1 fit
python3 -I an/disc_game.py discard discard_10 2 10
python3 -I an/disc_game.py pickup pickup_8 4 8
```

## The C port (what ships)

The game plays a C port of the strokes, the lifts, the placement and the slide into the hand: `../c/models.c` (`m_stroke`, `m_lift`, `m_place`, `m_plift`, `m_insert`), line for line from the Python here, with the measured tables copied into `../c/tables.h` by `../c/tables.py`.
`../soundgen.c` lays the parts out at the game's timing (`SWEEP` is the lift and `PICKUP` the soft lift, each with its strokes as a prelude of their own (1-5 by the cue's cards, uncompressed) played before it in full over whatever is sounding; `PILE` the placement; `INTO_HAND` the slide, its own cue at the pickup's landing) and writes the takes at build time; `../../../docs/AUDIO.md` ("The card sounds") is the guide.
Measured with `../throwin/an/measure.py`, every part is inside the Python takes' range on every measure but the slide into the hand's crest (15.58 against 15.57), and outside the recordings where the Python is.
`make -C .. parts` writes the C takes alone (12 at 48 kHz) to compare against the Python's `synth/`.
The Python stays the reference and the analysis; change a model here first, then its port.

The levels in the game are this model's own, with the offsets of `PART_DB` in `../soundgen.c` on top (the levels study of 2026-11-07: a card's landing ~25 dB over the grab and slide in one video, and the discard and pickup videos twice as far from the phone), and one common gain over the whole card set; `../../../docs/AUDIO.md` ("The balance between the parts") lists both.
