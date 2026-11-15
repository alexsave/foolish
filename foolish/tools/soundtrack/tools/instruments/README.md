# Instrument inventory tools

Tools that split the separated stems of a clip into voices, measure each voice, and guess its instrument family with a rule table.
All outputs are measurements and rule-table guesses; nothing here describes how audio sounds.
Reuse: stems come from `../transcribe/stems.sh`, notes from `../transcribe/notes.sh`, bar grids from the earlier DSP reports of `../analysis/`, envelopes and the drum onset classifier from `../transcribe/envelopes.py` and `window_report.py`.

## Files

- `voices.py` splits notes in a window into voices (rules below).
- `describe.py` measures one voice on its stem audio (pitch, envelope, spectrum, brightness over time, vibrato, glides, detune and beating, stereo, entrance), or a drum voice.
- `guess.py` is the rule table from measurements to an instrument family with a confidence and an evidence line, and prints the instrument words a prompt asked for.
- `inventory.py` runs the three over a list of (clip, window, verdict, note) and writes INDEX.md, INDEX.tsv and per-voice page, png and wav excerpt.
- `inventory.py` options: `--list` (repeatable), `--sequences-for TSV` (whole-window per-bar bass, top line, chord and drum-pattern pages for keep rows), `--override SUBSTR:min_notes=3,vocals_db=-35` (per-clip thresholds, used for mo_14 so the quiet hum voice gets a page).
- `chorus.py` writes the se_12 finding (called by `inventory.py --chorus`).
- `test_instruments.py` is the deterministic synthetic test (about 3 s).
Run everything with the soundtrack venv python, for example `/Users/alex/Dev/soundtrack-tools/venv/bin/python -I inventory.py ...`.
Each file header lists inputs, outputs, an example command and failure modes.

## Voice rules (voices.py)

- bass stem: one voice.
- vocals stem: one voice if its RMS in the window is within 15 dB of the mix, else none.
- other stem, notes assigned once in this order: "chords or pad" (three or more notes starting within 40 ms, or notes longer than 1.5 beats overlapping another note by over 50 ms), "short notes" (under 0.2 s), "top line" (highest sounding note of the stem at its onset), "low-mid line" (more than 12 semitones under the median top-line pitch).
- a voice needs at least 6 notes; notes under 60 ms are ignored.
- drums stem: low (under 150 Hz), mid (150 Hz to 2 kHz), high (above 5 kHz) by the onset classifier of `window_report.py`.

## Measurement notes (describe.py)

- Notes that sound alone in their stem are preferred; with fewer than 5 of them the voice is measured on overlapped notes and the guess is "undetermined".
- Fundamental tracking is harmonic summation over harmonics 1 to 4 starting at the transcribed pitch, so transcription rounding does not matter and glides are followed.
- Vibrato needs notes of 0.6 s or more; depth is the amplitude of the detrended pitch track in cents (a 30 cent vibrato reads 30).
- A glide into a note needs a deviation of 40 cents or more at the note start and at least 3 frames (30 ms) between 25 cents and 85 percent of that deviation; a step is not a glide.
- Formant-like peaks use a Gaussian smoothing of max(350 Hz, f0) over the harmonic levels, so for f0 above about 500 Hz the harmonic spacing is wider than a formant and the result is inconclusive.
- Noise share is the energy not within 40 cents (or 1.5 window bins) of a harmonic, inside 0.5 f0 to 10 kHz.

## Guess rule table (guess.py)

Weights in brackets; fraction = score over the evaluable maximum; confidence high at 0.75 or more, medium at 0.55, low at 0.40, else undetermined; a second family within 0.10 is listed as well.

| family | rules |
|---|---|
| plucked string | attack under 25 ms (2); brightness falls (2); sustain at most -6 dB (2); inharmonicity 1 to 40 cents (1); no vibrato (1); release under 0.6 s (1) |
| struck (piano, mallet) | attack under 20 ms (2); sustain at most -10 dB (2); inharmonicity 3 cents or more rising with pitch (2); brightness falls (1); no vibrato (1) |
| bowed string | attack 40 to 400 ms (2); vibrato 4.5 to 7.5 Hz in 30 percent of long notes (2); glide or portamento 20 percent (1); 8 or more harmonics within 30 dB (1); noise share over 0.04 (1); sustain within 6 dB (1) |
| blown reed or brass | attack 30 to 120 ms (1.5); centroid rises with level, correlation over 0.4 (2); odd-to-even over 6 dB, clarinet-like (1); 2 to 5 kHz share over 0.08, brass-like (1); sustain within 6 dB (1) |
| flute-like | 4 or fewer harmonics within 30 dB (2); noise share over 0.08 (2); attack 30 to 150 ms (1); median pitch above MIDI 72 (1); sustain within 6 dB (1) |
| sung voice or choir | vocals stem (2); vibrato 4.5 to 7.5 Hz (1.5); formant-like peak not moving with f0 (2); noise share over 0.04 (1); glide or portamento 20 percent (1); polyphony over 1.5 (0.5) |
| saw-type synth lead | slope -9 to -3 dB per octave (2); inharmonicity under 6 cents (1.5); no vibrato (1); odd-to-even -8 to 2 dB (1); noise share under 0.05 (1); brightness static or swept (0.5) |
| square-type synth | odd-to-even over 10 dB (3); inharmonicity under 6 cents (1); sustain within 6 dB (1); no vibrato (0.5) |
| sine or sub bass | 2 or fewer harmonics within 30 dB (3); median pitch under MIDI 45 or bass stem (1.5); noise share under 0.05 (1); no vibrato (0.5) |
| supersaw stack | fundamental peak excess width 20 cents or more (2); beating in 30 percent of long notes (1.5); side/mid over -6 dB (1.5); saw slope (1); no vibrato (1) |
| FM or bell-like | inharmonicity 25 cents or more (2.5); sustain at most -10 dB (1.5); attack under 25 ms (1); no vibrato (0.5) |
| pad | attack 150 ms or more (2); release 300 ms or more (2); side/mid over -9 dB (1); chords-or-pad voice or polyphony 2 or more (1); median note length 1.5 s or more (1) |
| drums | low, mid, high by band; "kick-like", "snare, clap or tom-like", "hat, shaker or cymbal-like", always at most medium |

"Undetermined" is returned when fewer than 5 notes sounded alone in their stem, with the measurements.
The instruments the prompt asked for are printed as "the prompt asked for" (words in the prompt, negated clauses listed separately), never as something heard.

## Known limits

- The stems leak, so a voice can hold another instrument.
- basic-pitch never emits notes under about 128 ms and invents ghost notes; the high register of "top line" often holds ghosts.
- The vocals rule is applied to the whole window, so a vocals-stem line that is loud only for part of a long window gives no voice (se_12 35-end; the 64-78 s line is measured in `se_12_chorus.md`).
- The guess table is judgement, not a trained classifier, and has no family for guitar strum, accordion, organ or layered synths.
- Dense windows have few notes alone in their stem, so many voices are "undetermined" by rule.

## Test and mutation checks

`test_instruments.py` prints one PASS or FAIL per named check (49 checks) and exits non-zero on any failure.
Each mutation below was applied to a scratch copy of the folder (with `../transcribe` beside it), the test run, and the failing checks noted; the unmutated test passes all checks.

| mutation | failed checks |
|---|---|
| swap centroid windows (0-50 ms and last third trade places) | saw_brightness_falls, saw_last_over_early_centroid, pluck_brightness_falls |
| harmonic picker reads even harmonics as odd | square_odd_even_high, square_guess |
| glide detection disabled | glide_detected, glide_cents_near_200, glide_ms_near_150 |
| register split set to zero (octave rule removed) | voices_register_gap_over_octave, voices_counts, voices_inner_notes_unassigned |
| vibrato search band moved from 3.5-9.5 Hz to 7.5-12 Hz | vibrato_rate_6hz_15pct, vibrato_depth_30c_15pct, vibrato_share_high, vibrato_delay_near_0p25 |
| sustain level sign flipped | pluck_sustain_decays |
| inharmonicity deviation forced to zero | pluck_inharm_measured |
| vocals RMS threshold moved from -15 dB to -45 dB | vocals_rule_quiet_none |
| minimum notes per voice set to 1 | voices_min_6_notes |
| side and mid swapped | stereo_mono_narrow |
| noise share counts harmonic bins instead of the rest | noise_share_clean_low |
| entrance silence test inverted | saw_entrance_silent_before |
| drum pitch-drop windows swapped | drum_pitch_drop |
| guess: square rule inverted | square_guess |
| glide length scaled by 0.1 | glide_ms_near_150 |
| harmonic slope fitted against log10 instead of log2 | saw_slope_minus6 |
| low drum voice takes the mid class | drums_low_voice_kicks |
| vibrato depth halved | vibrato_depth_30c_15pct |
| guess: saw slope rule widened to any slope | steep_slope_not_high_saw |

One first attempt survived and was strengthened: widening the saw slope rule to any slope changed only the confidence of a steep-spectrum tone, so a check that a -18 dB per octave tone does not get a high-confidence saw-type guess was added (it now fails under that mutation).
