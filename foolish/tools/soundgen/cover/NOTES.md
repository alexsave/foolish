# Cover and pass

## Cover (here)
Alex: "cover is the same two finger grab, slide out, but the landing is on a card so it sounds different".
- **Landing:** modelled from his cover reps 1 2 3 5 8 10 12 13 14 of IMG_2934 (`parts/real_k.wav`, cut -80..+400 ms).
  - Reps 9 and 11 did not match their own source and were dropped.
- **How it differs from the throw-in landing:**
  - brighter: 8-15 dB more above 2 kHz, with the low and high bands level;
  - less body;
  - about half as long: above -10 dB for 13 ms against 23;
  - a tight pair of strikes.
- **Table:** the same as the throw-in's, ringing at 234 Hz, 3.3 dB over its neighbours. A 703 Hz peak shows in FFT bins but not as a band level, so it is not modelled.
- **`an/cov_syn.py`:** the throw-in synth with every table re-measured, slap at -6 dB, plus a spectral fit (`an/cov_fit.npy`).
  - Approved files: `cover3`, seed 7.
  - Inside the reps on every measure except kurtosis at 1-4 kHz (13.1 against a maximum of 12.4).
- **Whole cover:** `../throwin/an/throwin_game.py SOUNDGEN NAME SEED COUNT GAP 'cover/synth/cover3_*.wav' 4 6`.
- **Alex on hearing it:** "quiet as fuck. the two finger grab is too quiet. the shape of the sounds are good, we'll work on equalizing". Level balancing across all the sounds is still to do.

## Pass (../pass)
Alex: "2 finger grab, slide out, throw in landing, and then a shfff as you slide".
- **The "shfff":** modelled from IMG_2936's 6 slides. Two fingers press the card flat and slide it across the cloth (`../pass/parts/pass_k.wav`, onset at 0.1 s).
- **What it is:**
  - broadband hiss peaking at 2.4 kHz, swelling to about 150 ms and down 20 dB by about 280 ms;
  - no pitch track (unlike the thumb slide);
  - snags clustered at the start and the stop;
  - a trace of weave flutter at 57 and 96 Hz;
  - low table knocks near 200 Hz as the fingers land and as the card stops. Modelled on the game's 229 Hz table.
- **`../pass/an/pass_syn.py`:** approved files `pass7`, seed 4.
  - Inside the reps on every measure except flatness (0.50 against 0.43-0.48: the real hiss is very slightly more coloured).
  - `pass_check.py` holds the onset-aligned envelope, snag and weave measures.
- **Whole pass:** `../pass/an/pass_game.py`. The shfff starts 150 ms after the landing. That gap and all levels are placeholders.
- Alex had not reacted to the pass yet when he paused.

## Next
1. Alex's review of the pass.
2. Level balancing ("equalizing") across every sound.
3. Discard and pickup: first try in `../sweep` (NOTES.md there).
4. The C port: done (below).

`../round_demo.py` assembles a 4-player deal, the trump flip and three attack/cover pairs from all the models.
It runs from the scratchpad layout; paths need adjusting to run here.

## The C port (what ships)

The game plays a C port of cover3 and pass7 (`../pass`): `../c/models.c` (`m_cover`, `m_pass`), line for line from the Python here, with the measured tables copied into `../c/tables.h` by `../c/tables.py`.
`../soundgen.c` lays the parts out at the game's timing (`COVER` is the cover's landing after the same `LIFT`; a transfer is `PLACE` and then `PASS`, the shfff from 150 ms after the landing, told from an attack by the event's msg) and writes the takes at build time; `../../../docs/AUDIO.md` ("The card sounds") is the guide.
Measured with `../throwin/an/measure.py`, both are inside the Python takes' range on every measure; against the reps the cover is just outside on click width and 1/20 ms roughness, the pass on flatness (as Python), 1-4 kHz kurtosis and 4-12 kHz crest.
`make -C .. parts` writes the C takes alone (12 at 48 kHz) to compare against the Python's `synth/`.
The Python stays the reference and the analysis; change a model here first, then its port.
