# Discard and pickup (first try, 2026-11-07, not yet heard by Alex)

Alex: discard is "a bunch of passes playing at the same time, with roughly a few instances of difference in where they start, and all end in the same round. then several rounds".
The number of shfffs is the number of cards, "like a bunch of cards being swept on the table".
Pickup is "similar, but then with a flip sound, and a small sound of cards tapping each other when they are placed into the hand", the taps like a cover but quieter.

## How `an/sweep_game.py` builds them
- **Strokes:** the cards are split over 1-3 sweeps of the hand, about 4 cards a stroke.
- **In a stroke:** the cards join in 2-3 clusters, each starting a little later, and they all stop when the hand stops.
  - Each card is the pass synth (`../pass/an/pass_syn.py`) time-stretched to how far it travels.
  - The fingers land once per stroke, so only the first card has the landing knock.
  - The other cards ride on cards, not cloth: they are 3-6 dB quieter, and their stop knocks are 12 dB down.
- **Pickup:** after the sweep, the flip (`../flip` flipE), then 2-3 cover landings (`../cover` cover3) 10 dB under a cover, as taps.
- **Timing:** Alex: "reasonable timings as you'd expect in a card game". The numbers are in the script's docstring.
- **Levels:** on the deal's scale, like everything else; still to be balanced.

## Reproduce
```
cd ../flip && python3 -I an/flip_syn.py flipE 3
cd ../cover && mkdir -p synth && python3 -I an/cov_syn.py cover3 7
cd ../sweep && mkdir -p synth
python3 -I an/sweep_game.py discard discard_6 1 6
python3 -I an/sweep_game.py discard discard_12 2 12
python3 -I an/sweep_game.py pickup pickup_4 3 4
python3 -I an/sweep_game.py pickup pickup_10 4 10
```
