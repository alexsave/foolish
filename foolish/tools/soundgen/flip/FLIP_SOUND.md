# The trump flip and the deck set down

These are the last two ingredients of the opening deal.
After the cards are dealt (see `../deal/DEAL_SOUND.md`), the top card of the stock is flipped face up as the trump, and the stock is set down on top of it.
Both are modelled on Alex's recordings from 2026-11-02:
- IMG_2951: 7 flips off the top of a held deck;
- IMG_2950: 6 times setting the deck down on a face-up card.

`parts/` holds the cut reps:
- the flips, with the snap at 0.35 s;
- the deck placements, with the thud at 0.2 s;
- the slide-only and whup-only cuts that `an/flip_an.py` makes;
- a room-tone stretch from each recording (for A/B listening only, never in the synth).

## The flip (an/flip_an.py, an/flip_spec.py)
The video shows the thumb pushing the top card off sideways, and the other hand taking it and turning it over.

### Slide
- It is a hiss of its own, not the deal's thumb slide.
  - Measured against the deal's slides, it is 20 dB louder with almost no low end, and its centroid is 5 kHz against 1.6 kHz.
  - Part of that is the phone being a few centimetres from the cards; part is the motion.
- Length: 54-118 ms.
- Spectrum: peaks near 7 kHz.
- Resonance: one LPC resonance falls from 3.6 kHz to 1.5 kHz across the slide (reps 1, 3, 4, 5). The deal's thumb slide rises.
- Envelope: rises to a peak 9-47 ms before the snap, then falls about 12 dB in 20 ms as the card leaves the thumb.
- Level: its peak is steady at -25 to -20 dBFS in every rep.

### Snap
- 4 of the 7 reps have a hard click (1.3-2.9 ms; a partner at +1.6 ms or a tick at +16 ms sometimes). It is 12-20 dB over the slide in the recording.
- The other 3 release softly, with no click; the slide is then the loudest thing.

### Whups (the "floppy" part)
- After the snap come 1-4 soft, broad low pulses (about 320 Hz wide, about 18 ms long), the first at +19 to +58 ms, then every 40-55 ms.
- Each pulse is lower than the last (565, then 372, then 287 Hz), and the later ones are 4-17 dB down.
- This is the card flapping as it turns, slowing down.

### Beds
- Low and high beds after the snap, on their measured curves.
- The tail keeps falling to about -60 dB by 500 ms. Alex: the first version "sounds a bit cut off", because the beds stopped dead at 250 ms.

## The deck set down (an/deck_an.py)
1. **Edge clatter:** a dense, dark burst about 20 ms before the deck lands flat (5 ms rise, 5 ms decay), with 2-9 bright contacts in it. Its 1-15 kHz peak is 14-20 dB under the thud's low peak. In 1 placement of 6 the deck slapped flat and the brightness came at the thud instead.
2. **First touch:** a low knock 10-16 ms before the thud, about 4-8 dB under it (the corner landing), then a dip.
3. **The thud:** almost all low end, then the table's modes.
   - The recording's table rang at 196 Hz in 5 of 6 placements (211 Hz in the sixth), 12-20 dB over the smoothed spectrum, with weaker modes at 287 and 397 Hz.
   - The modes are not in the room tone.
4. **The settle:** a quiet high bed about 41 dB under the thud, fading over about 250 ms.

## The synths (an/flip_syn.py, an/deck_syn.py)
Each part is built from its own measured table, then checked against the reps with `../throwin/an/measure.py` (22 measures, plus spectrum and envelopes).
Both end with a band-by-band spectral fit (`an/flip_fit.npy`, `an/deck_fit.npy`).
Below 500 Hz the deck fit is unsmoothed, because a 3-band smoothing pushed the 239 Hz band the wrong way around the table mode.

### What the measurements found on the way
- **The 1-2 kHz gap:** the beds left 1-2 kHz empty (as the throw-in once did). Fixed by meeting at 1.5 kHz.
- **Smeared contact lows:** 1.5 ms analysis windows make the contacts' low end look flat. Uncorrected, a synth contact out-peaked the thud.
- **Double-counted modes:** the bed spectrum already holds the table's modes, so it is smoothed below 500 Hz before the resonators are added on top.

### Alex's review
- **"Too much echo" on the deck:** the table modes are now 12 dB down and decay about twice as fast (9, 11 and 13 Hz wide). The low tail sits 2 dB under the measured one.
- **"Too much clapping", then "still a lot of slapping", on the flip:** a slap is the snap's low-mid body. Three changes fixed it:
  - the snap is cut 12 dB below 1.5 kHz;
  - its dark release burst sits at -24 dB;
  - a hard snap is 0-6 dB over the slide, not the recording's 12-20.
- **Whups:** they are held 5 dB up so the snap's drop does not take them with it.
- **The table:** "ring at 229 Hz", the throw-in's table, so the game has one table. The deck's modes keep their ratios and move up by 229/196, to 229, 335 and 464 Hz. The low part of the fit moves with them.

**Results.**
- **The flip:** inside the reps' range on every measure except kurtosis at 1-4 kHz (12.0 against a maximum of 10.9). The snap is now a thin tick, which is what Alex asked for.
- **The deck:** inside on every measure except two:
  - its fall to -20 dB, 0.5 ms slower;
  - its centroid, higher because the modes moved up to 229 Hz.

  Its spectrum is within about 4 dB below 10 kHz.

## At the game's timing (an/flip_game.py)
The trump's step starts as the card leaves the stock (`ANIM_EVT_FLIPPED`). It flies `ANIM_TIME_MS` = 500 ms, and `ANIM_CUE_FLIP` is struck at the landing.
1. The flip, with its snap at the step start.
2. The landing at +500 ms: a throw-in (`../throwin`, Alex: "it goes on the table, similar to a throw in").
3. The deck set down 350 ms after the landing (Alex: "350 ms after trump lands is fine"). The game has no event for this yet.

Levels are on the deal's scale (`../deal/an/deal_game.py`, g = -34 to -32 dBFS):
- the flip at g-5, the grab's level;
- the landing at g-12 to g-9;
- the deck at g-6 to g-3, the heaviest.

## Reproduce
```
cd foolish/tools/soundgen/throwin && mkdir -p synth plots && python3 -I an/throw_syn.py throw9 7
cd ../flip && mkdir -p synth plots
python3 -I an/flip_syn.py flipE 3                   # the approved flips (snap at 0.35 s), plus slide and whup cuts
python3 -I an/deck_syn.py deck229c 5                # the approved deck set downs (thud at 0.2 s)
python3 -I an/flip_game.py flip_game_e 4 4 2.5      # four trump flips at the game's timing
python3 -I ../throwin/an/measure.py flipE 'parts/flip_?.wav' 'synth/flipE_[0-9]*.wav'
python3 -I ../throwin/an/measure.py deck229c 'parts/deck_?.wav' 'synth/deck229c_*.wav'
```
These regenerate exactly the files Alex approved (checked bit for bit).
`an/flip_an.py`, `an/flip_spec.py` and `an/deck_an.py` re-measure the reps and rewrite the tables.

## The C port (what ships)

The game plays a C port of flipE and deck229c: `../c/models.c` (`m_flip`, `m_deck`), line for line from the Python here, with the measured tables copied into `../c/tables.h` by `../c/tables.py`.
`../soundgen.c` lays the parts out at the game's timing (`TURN` is the flip, its snap as the trump leaves the stock; `FLIP` is the throw-in landing as it lands, with the deck set down baked in 350 ms later, since nothing on screen moves there) and writes the takes at build time; `../../../docs/AUDIO.md` ("The card sounds") is the guide.
Measured with `../throwin/an/measure.py`, both are inside the Python takes' range on every measure; against the recordings the flip is outside on rise, centroid and click width (Python's 1-4 kHz kurtosis comes back inside), the deck on its fall to -20 dB and centroid (as Python) and on 4-12 kHz kurtosis.
`make -C .. parts` writes the C takes alone (12 at 48 kHz) to compare against the Python's `synth/`.
The Python stays the reference and the analysis; change a model here first, then its port.

The levels in the game are this model's own, with the offsets of the levels study on top, all of it in each part's line of `../tuning.tsv` (the levels study of 2026-11-07: a card's landing ~25 dB over the grab and slide in one video, and the discard and pickup videos twice as far from the phone), and one common gain over the whole card set; `../../../docs/AUDIO.md` ("The balance between the parts") lists both.
