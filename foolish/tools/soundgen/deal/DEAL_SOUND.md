# The deal sound

How one dealt card sounds, built from Alex's own recordings of dealing (2026-11-01), measured part by part and synthesized so that every card varies the way the real takes vary.
This is a PROTOTYPE in Python (measurement and synthesis) beside the C `soundgen`; the shipped version will be a port of these recipes into `soundgen.c`.

## What a deal is (from the recordings)

Alex dealing slowly (IMG_2942) and as fast as he can (IMG_2943):

- At speed the hands repeat thumb slide, grab, thumb slide, grab: the left thumb slides the top card over, the right hand grabs it, it flies (silently), it lands on the felt.
- Slow take: the grab comes 0.30-0.32 s after the slide's onset; the landing 0.45-0.58 s after the grab.
- Fastest real pace: about 476-495 ms a card; the game deals faster (375 ms a card), which is fine.
- At speed a card's landing falls on top of the next card's grab, which is why it vanished from averages.
- Rule (Alex): the thumb slide and the grab never overlap; the flight and the landing may overlap the next card.

## The three parts

### Thumb slide (`an/thumb2.py`, version 9: `thumb9`)

Reference: `parts/real_thumb_1..5.wav`.

- Spectrum: noise coloured by the five slides' average 1/12-octave spectrum, which is "hollow": a low body 200 Hz-1 kHz, a scoop at 1.5-5 kHz (deepest near 4.5 kHz), a peak at 7-9 kHz.
- The rising tone: two resonances that move, found with LPC (order 20 at 24 kHz, 5 ms hop): the low one dips and rises (1436 -> 1051 -> 1305 Hz), the high one rises (2627 -> 3075 -> 3244 Hz). They are an air gap between the sliding card and the deck shrinking, and Alex's accelerating thumb. Synth: two time-varying two-pole resonators H(z) = (1-r) / (1 - 2r cos(w) z^-1 + r^2 z^-2), r = exp(-pi B / fs), w updated per sample along the measured tracks, timed against the envelope (the audible part), not the clip.
- Envelope: the real slides' median curve itself (dB re peak, 10 ms RMS of 1-15 kHz): a long swell from -23 dB 120 ms before the peak, a 20-30 ms catch about 6 dB above it (the edge clearing the deck), then about 2.3 dB per 10 ms down. Per variant: time x0.85-1.15, +-1.5 dB per point.
- Texture: a slow (15-50 Hz) stick-slip wobble small enough that the roughness matches (about 1.8 dB).
- Levels: peaks -41.5 to -35.5 dBFS (the phone was very close; it is scaled in the deal below).

What went wrong on the way, so nobody repeats it:
- Octave-band noise with a symmetric swell sounded like maracas: too bright, too smooth, no moving resonance.
- Strong narrow resonances sounded like sandpaper: they stood ~4 dB proud at 1 and 3 kHz, the ear's scratch zone, while the airy 5-10 kHz was short.
- A hand-drawn ADSR ended too abruptly: the real release is about 100 ms at about 2 dB per 10 ms; ours fell twice as fast.

### Grab (`an/grab2.py`: `grab2`)

Reference: `parts/real_grab_2..5.wav` (grab 1 dropped: faint, mostly low).

- Not one envelope: 6-11 separate clicks over about 70 ms (`an/clicks.py`): 3-7 approach clicks from -40 to -4 ms (12-26 dB down, the fingers catching the edge), the main click (0.7-1 ms wide) with a partner 1-7 ms away (2-9 dB down), sometimes 1-2 late clicks at +15-30 ms.
- Each click: a short burst (0.05 ms rise, exponential fall) shaped by the measured click spectrum: broadband, flat to 1.5 kHz, -3 dB at 2.4 kHz, -6 dB at 4-6 kHz, -12 dB at 10 kHz. The clickiness is sharpness in time, not treble.
- Between clicks: a quiet, low hand bed (centred near 240 Hz) in two bands, because the highs die faster than the lows: highs -11 dB at 20 ms to -32 dB at 100 ms, lows -6 dB at 20 ms to -19 dB at 130 ms. One shared envelope left a run-on hiss.
- Matches: kurtosis ~10, flatness ~0.35, band decays within ~1-3 dB.
- A smooth noise burst under one envelope sounded like maracas again; the averages hid the clicks.

### Landing on felt (`an/land_syn.py`: `land1`)

Reference: `parts/real_land_4.wav` (Alex: the best), varied within landings 1, 2, 3 and 5.

- A double main contact (two, about 1 ms apart: edge, then face), a follow-up at +5-15 ms (4-8 dB down), a pre-contact at -8 to -25 ms (9-20 dB down, 70% of the time); each contact 2-3 ms wide with a softer onset than a grab's click (felt).
- Dark: the contact spectrum peaks at 240-380 Hz, -13 dB at 1 kHz, -19 dB at 2.4 kHz, a notch near 4 kHz, almost nothing above 6 kHz.
- A two-band bed on landing 4's decay curves; the highs vanish almost at once.
- Matches: spectrum within 1.7 dB RMS, kurtosis 9.4 vs 9.2.

## The deal (`an/deal_game.py`)

- Game timing: the grab at T (the card leaves on screen), the landing at T + 350 ms (ANIM_DEAL_CARD_MS, it lands on screen), the thumb slide's peak 130 ms before the grab so its audible part is over by the click.
- Levels (Alex, by ear): grab loudest, slide between, landing quietest: grab peak -37 to -39 dBFS (a base of -34..-32 less 5 dB), slide 6-9 dB under the base, landing 13-16 dB under the base.
- Seats (Alex): a landing's level follows its distance from the listener's head (inverse-distance law, -6 dB per doubling), seats on a circle (radius 0.5 m) round the dealer, the head 0.40 m above the table, one's own cards 0.15 m in front: at 4 seats, own 0 dB, left and right -4.7 dB, across -6.8 dB. A flat 2D model (ears at the table, own cards 0.25 m away) made across -12 dB, which Alex found far too quiet.

## How it was measured (tools in `an/`)

- `reps.c`: cut a take into its reps (150 ms smoothed envelope, peaks a minimum gap apart).
- `envcmp.py`, `adsr.py`, `tailcmp.py`, `land_tail.py`: envelopes aligned on the peak, per band.
- `pitch.py`, `lpc.py`: resonance tracks over time (LPC) and the centroid.
- `grit.py`, `grain.py`: flatness, roughness (1 ms vs 20 ms), micro-roughness (0.25 vs 2 ms), kurtosis, crest.
- `clicks.py`, `land_clicks.py`, `grab_an.py`: click detection and click/bed spectra.
- The rule that came out of this: put real and synth spectrograms side by side and fit to them before tuning any number; averages hide structure (clicks, moving resonances, two-band decays).

## Reproduce

From this folder: `mkdir -p synth plots`, then `python3 -I an/grab_an.py`, `python3 -I an/clicks.py`, `python3 -I an/land_clicks.py` (tables), then `python3 -I an/thumb2.py`, `python3 -I an/grab2.py`, `python3 -I an/land_syn.py` (variants into `synth/`), then `python3 -I an/deal_game.py 0.375 deal_4p_seats 9 24 4` (a 4-seat opening deal; arguments: pace s, name, seed, cards, seats).
