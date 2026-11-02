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

## Reproduce
```
cd foolish/tools/soundgen/throwin && mkdir -p synth plots
python3 -I an/throw_an.py                      # re-measures the reps (rewrites the two spectrum tables)
python3 -I an/throw_syn.py throw9 7            # 12 variants into synth/
python3 -I an/measure.py throw9 'parts/real_*.wav' 'synth/throw9_*.wav'
```
