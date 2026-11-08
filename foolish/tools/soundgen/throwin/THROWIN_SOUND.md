# The throw-in sound

A single card thrown onto the cloth, modelled on Alex's recording IMG_2933.
The reps he kept are 2-8, in `parts/`, each cut from -80 ms to +400 ms around its peak.
The video shows the card arriving tilted: its leading edge strikes first, then the card slaps flat.

## What the real throw-in is (an/throw_an.py)
- **Strikes:** about 3-4 per throw within 60 ms, 1-3.5 ms wide.
  - The loudest is the slap.
  - An edge strike comes 5-14 ms before it, -16 to -3 dB.
  - Sometimes there is a partner 1-3 ms after it.
  - Sometimes there is a small bounce at +13 to +35 ms.
- **Low body:** 150-1000 Hz. It outweighs the strikes (+4 dB at +6 ms) and decays to about -25 dB by 170 ms.
- **High bed:** 2-15 kHz. It dies much faster than the low body.
- **Table mode:** 222-238 Hz, about 6 Hz wide, 3-6 dB over its neighbours from +10 to +90 ms in every rep.
  - It is not in the room tone, so it is the table.
  - It is subtle; the earlier "hollow wooden" failure came from modes far stronger than this.
- **Spectrum:** steep and dark. Spectral flatness 0.02-0.08 measures that tilt, not ringing (room tone reads 0.44).

## The synth (an/throw_syn.py)
Each throw is built from these layers:
1. **Strikes** placed as above, shaped by the measured strike spectrum.
   - They are tilted 3 dB/oct darker above 1 kHz and 18 dB/oct above 7 kHz.
   - The 1.5 ms analysis windows smear the low end, so the raw strike spectrum came out too flat.
2. **A slap burst:** noise with a 0.8 ms rise and a 3-5 ms decay, at -6 dB re the strikes, tilted 10 dB/oct above 700 Hz.
   - Without it, the 1-4 kHz kurtosis read 13 against the real 6.
3. **The two beds** on their measured curves, in dB re the loudest strike's 3-15 kHz 0.25 ms level.
4. **The table mode:** a two-pole resonator at 222-238 Hz, 6 Hz wide, set to 4.5 dB over its neighbours.
5. **One band-by-band spectral fit**, `an/tin_fit.npy`, taken from measure.py's B-A spectrum.

Every number varies per throw within the measured ranges.
Peaks range from -21 to -10 dBFS, like the real reps.

## How it was checked (an/measure.py, the general comparison tool)
`measure.py NAME 'realglob' 'synthglob'` compares two sets of clips on 22 measures:
- level and crest, full band and per band;
- durations, rise and fall times;
- low-high balance and centroid;
- click count, width and span;
- kurtosis, crest and micro-roughness in 1-4 kHz and 4-12 kHz;
- 1/20 ms roughness;
- spectral flatness;
- the 1/12-octave spectrum and the 1-15 kHz and two-band envelopes.

It also writes spectrograms and overlays to `plots/NAME_cmp.png`.

**Results.** Version 9 (seed 7) sits inside the real reps' range on all 22 measures.
Its spectrum is within 2.5 dB in every band except 239 Hz, which is 4 dB short.
The old throw-in had no strikes and highs that were too smooth and 8 dB too bright.

**Remaining residual:** the real waveform has more low-frequency swing under the strike.
That is inside the real range but at its edge.
Pushing it further worsened the low-high balance.

## Alex's review
See ALEX_NOTES.txt. He picked variants 4, 5 and 6 of seed 7.
Measured, those three have a centroid of 630-680 Hz (the real median is 604 Hz).
The variants he passed over were darker (447-485 Hz) or brighter (748 Hz).
Next: keep the variation within a centroid of about 580-700 Hz.


## The whole throw-in: four steps (Alex, 2026-11-02)
"The person grabs a card, slides it out, throws it in. Like the deal, it has 4 steps."
1. **Grab:** two fingers take a card in the hand. It is very quiet and is modelled from its own recording (below).
2. **Slide out:** the thumb slides the card out from the other cards. This is the deal's thumb slide (`../deal`).
   - The grab comes right before it (Alex: "right before the thumb slide").
3. **Flight:** the card leaves the hand and flies ANIM_TIME_MS = 500 ms (Alex: "500ms flight time is fine").
4. **Landing:** the aggressive throw-in above, where the game strikes ANIM_CUE_PLACE.

`an/throwin_game.py SOUNDGEN NAME SEED COUNT GAP` lays them out on the deal's level scale g (-34 to -32 dBFS):
- the grab at g-14, 110 ms before the slide's peak;
- the slide at g-(6..9), its peak 130 ms before the card leaves (as in the deal);
- the landing at g-(3..5).

## The two-finger grab (an/tgrab_an.py, an/tgrab_syn.py)
The recording is IMG_2952. `parts/grab_1-8.wav` hold the 8 grabs, each with its peak at 0.2 s; `parts/grab_room.wav` is its room.

**It is its own sound, not a quieter deal grab.** Compared with the deal's grab (`measure.py`), it is:
- darker: centroid 350 Hz against 975 Hz, with 10-23 dB less above 1 kHz;
- shorter: 15 ms above -10 dB against 30;
- less clicky: about 2 clicks against 7.

**Clicks.**
- The main click is 0.75-1.9 ms wide.
- In 7 of 8 grabs, a softer pre-click comes 2.6-11 ms before it (-12 to 0 dB).
- In half, a partner follows 1.5-1.8 ms after it (-12 to -9 dB, about 3 ms wide).

**Spectrum.** It peaks at 300 Hz and is 17 dB down by 1.2 kHz, with a small bump at 2.4 kHz.

**Envelope (re the full-band peak).**
- A soft lead-in over 15 ms.
- The contact.
- A low rustle about 20 dB down that lasts at least 80 ms.

The rustle is well above the room (the room's low band is about 36 dB under a grab's peak), so it is real.

**How it was checked.** Measured with the recording's own room under the synth (`an/with_room.py`), so that the tails compare like for like.
The shipped sound has no room in it.
With the clicks +3 dB and one spectral fit, every measure falls inside the 8 grabs.

## Reproduce
```
cd foolish/tools/soundgen/throwin && mkdir -p synth plots
python3 -I an/throw_an.py                      # re-measures the reps (rewrites the two spectrum tables)
python3 -I an/throw_syn.py throw9 7            # 12 variants into synth/
python3 -I an/measure.py throw9 'parts/real_*.wav' 'synth/throw9_*.wav'
python3 -I an/tgrab_syn.py tgrab2 9                 # the two-finger grabs
python3 -I an/with_room.py tgrab2 && python3 -I an/measure.py tgrab2 'parts/grab_?.wav' 'synth/tgrab2R_*.wav'
python3 -I an/throwin_game.py .. throwin_game1 2 4 2.0   # four whole throw-ins (needs ../deal/synth/thumb9_*.wav)
```

## The C port (what ships)

The game plays a C port of throw9 and tgrab2: `../c/models.c` (`m_throw`, `m_tgrab`), line for line from the Python here, with the measured tables copied into `../c/tables.h` by `../c/tables.py`.
`../soundgen.c` lays the parts out at the game's timing (`LIFT` is the grab and the thumb slide before the card leaves, `PLACE` the landing; for a move that opens its run (no time before the lift) tight takes put the grab 60 ms before the slide's peak and the peak 30 ms before the card leaves, played whole from that moment) and writes the takes at build time; `../../../docs/AUDIO.md` ("The card sounds") is the guide.
Measured with `../throwin/an/measure.py`, both are inside the Python takes' range on every measure; against the reps the throw-in is just outside on three (dur > -10 dB 17.5 against 18, rise 10 against 11, 4-12 kHz micro-roughness 4.26 against 4.23) and the grab on two (1-4 kHz micro-roughness, as Python; 4-12 kHz kurtosis).
`make -C .. parts` writes the C takes alone (12 at 48 kHz) to compare against the Python's `synth/`.
The Python stays the reference and the analysis; change a model here first, then its port.
