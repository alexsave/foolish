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

## At game pace (`an/disc_game.py`)
- One stroke per ~2.5 cards (1-5), each 0.8x a recorded stroke, 60-200 ms apart, a little louder as the pile grows.
- The lift 100-180 ms after the last stroke; the placement after a 500 ms flight (ANIM_TIME_MS).
- Pickup: the same strokes and lift, then 550-700 ms later (Alex: "a bit more time between flip and put into hand") 2-3 quiet cover taps into the hand.

## Reproduce
```
python3 -I an/cut.py DIR      # DIR holds d3046.wav d3047.wav d3048.wav (mono 48 kHz from the videos)
python3 -I an/disc_an.py
mkdir -p synth && python3 -I an/disc_syn.py d4 1 fit
python3 -I an/disc_game.py discard discard_10 2 10
python3 -I an/disc_game.py pickup pickup_8 4 8
```
